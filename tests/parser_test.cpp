// Parser tests. Plain assertions, no test framework, so the suite builds with
// nothing but Qt Core available.
#include "core/ConfigProject.h"
#include "core/NixFile.h"
#include "core/NixLexer.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

using namespace nixm;

static int g_failures = 0;
static int g_checks = 0;

static void reportFail(const char *what, const QString &detail, int line)
{
    ++g_failures;
    QTextStream(stderr) << "FAIL (" << what << ") at line " << line << ": " << detail << "\n";
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond))                                                                               \
            reportFail("CHECK", QStringLiteral(#cond), __LINE__);                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        const auto va = (a);                                                                       \
        const auto vb = (b);                                                                       \
        if (!(va == vb))                                                                           \
            reportFail("CHECK_EQ",                                                                 \
                QStringLiteral("%1 != %2").arg(toDebug(va), toDebug(vb)), __LINE__);               \
    } while (0)

static QString toDebug(const QString &s) { return QStringLiteral("\"%1\"").arg(s); }
static QString toDebug(int v) { return QString::number(v); }
static QString toDebug(bool v) { return v ? QStringLiteral("true") : QStringLiteral("false"); }
static QString toDebug(qsizetype v) { return QString::number(v); }

static NixFile fromText(const QString &text)
{
    NixFile f;
    f.setText(text);
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────

static void testLexer()
{
    const QString src = R"NIX(
{ pkgs, ... }:
{
  # a comment with a ] bracket
  a = "string with ${pkgs.foo} and a ] and a \" quote";
  b = ''
    indented ''${escaped} and ] here
  '';
  c = ./relative/path.nix;
  d = ../../up/two.nix;
  e = /absolute/path;
  f = <nixpkgs>;
  g = a // b;
  h = 4 / 2;
  i = "https://example.com/x";
}
)NIX";
    const auto toks = tokenize(src);
    int paths = 0;
    int strings = 0;
    int indent = 0;
    int comments = 0;
    bool sawUpdate = false;
    for (const Token &t : toks) {
        if (t.kind == TokKind::Path)
            ++paths;
        if (t.kind == TokKind::String)
            ++strings;
        if (t.kind == TokKind::IndentStr)
            ++indent;
        if (t.kind == TokKind::Comment)
            ++comments;
        if (t.isPunct("//"))
            sawUpdate = true;
    }
    CHECK_EQ(paths, 4);        // ./x, ../../x, /absolute/path, <nixpkgs>
    CHECK_EQ(strings, 2);
    CHECK_EQ(indent, 1);
    CHECK_EQ(comments, 1);
    CHECK(sawUpdate);

    // `4 / 2` must not become a path.
    bool sawDivision = false;
    for (const Token &t : toks)
        if (t.isPunct("/"))
            sawDivision = true;
    CHECK(sawDivision);
}

static void testImports()
{
    const QString src = R"NIX({ pkgs, ... }:

{
    imports = [
        ./hardware-configuration.nix

        # ── Common ───────────────────────────────────────────────
        ../../modules/common/nix.nix
        ../../modules/common/audio.nix

        # ── Services ─────────────────────────────────────────────
        ../../modules/services/docker.nix
        #../../modules/services/ollama.nix
    ];

    networking.hostName = "main-pc";
    system.stateVersion = "24.05";
}
)NIX";
    NixFile f = fromText(src);
    CHECK(f.hasImportsList());
    CHECK_EQ(f.imports().size(), qsizetype(5));
    CHECK_EQ(f.imports().at(0).text, QStringLiteral("./hardware-configuration.nix"));
    CHECK_EQ(f.imports().at(0).section, QString());
    CHECK_EQ(f.imports().at(1).section, QStringLiteral("Common"));
    CHECK_EQ(f.imports().at(2).text, QStringLiteral("../../modules/common/audio.nix"));
    CHECK_EQ(f.imports().at(3).section, QStringLiteral("Services"));
    CHECK_EQ(f.imports().at(4).enabled, false);
    CHECK_EQ(f.imports().at(4).text, QStringLiteral("../../modules/services/ollama.nix"));
    CHECK_EQ(f.importSections().join(QLatin1Char(',')), QStringLiteral("Common,Services"));

    // Enable a commented-out import; everything else must stay byte-identical.
    f.setImportEnabled(QStringLiteral("../../modules/services/ollama.nix"), true);
    CHECK(f.text().contains(QStringLiteral("        ../../modules/services/ollama.nix\n")));
    CHECK(!f.text().contains(QStringLiteral("#../../modules/services/ollama.nix")));
    CHECK(f.text().contains(QStringLiteral("# ── Services ─")));

    // …and back again.
    f.setImportEnabled(QStringLiteral("../../modules/services/ollama.nix"), false);
    CHECK_EQ(f.text(), src);

    // Add into a named section.
    f.addImport(QStringLiteral("../../modules/common/ssh.nix"), QStringLiteral("Common"));
    const int nixIdx = f.text().indexOf(QStringLiteral("common/ssh.nix"));
    const int svcIdx = f.text().indexOf(QStringLiteral("── Services ─"));
    CHECK(nixIdx > 0);
    CHECK(nixIdx < svcIdx);   // landed in the Common block, not at the end
    CHECK(f.text().contains(QStringLiteral("        ../../modules/common/ssh.nix")));

    f.removeImport(QStringLiteral("../../modules/common/ssh.nix"));
    CHECK_EQ(f.text(), src);
}

static void testPackages()
{
    const QString src = R"NIX({ pkgs, ... }:

{
    environment.systemPackages = with pkgs; [
        vim
        git
        uutils-coreutils-noprefix    # Rust coreutils
        kdePackages.filelight
        #obs-studio
        (python3.withPackages (ps: with ps; [ websockets ]))
    ];

    fonts.packages = with pkgs; [
        fira-code
    ];
}
)NIX";
    NixFile f = fromText(src);
    const NixList *list = f.findList(QStringLiteral("environment.systemPackages"));
    CHECK(list != nullptr);
    if (!list)
        return;
    CHECK_EQ(list->withExpr, QStringLiteral("pkgs"));
    CHECK_EQ(list->isPackageList, true);
    CHECK_EQ(list->entries.size(), qsizetype(6));
    CHECK_EQ(list->entries.at(0).expr, QStringLiteral("vim"));
    CHECK_EQ(list->entries.at(2).comment, QStringLiteral("Rust coreutils"));
    CHECK_EQ(list->entries.at(3).expr, QStringLiteral("kdePackages.filelight"));
    CHECK_EQ(list->entries.at(4).enabled, false);
    CHECK_EQ(list->entries.at(4).expr, QStringLiteral("obs-studio"));
    CHECK_EQ(list->entries.at(5).expr,
        QStringLiteral("(python3.withPackages (ps: with ps; [ websockets ]))"));

    CHECK_EQ(f.packageLists().size(), qsizetype(2));

    f.addPackage(QStringLiteral("environment.systemPackages"), QStringLiteral("htop"),
        QStringLiteral("added by nixos-manager"));
    CHECK(f.text().contains(QStringLiteral("        htop  # added by nixos-manager")));
    // Appended after the last entry, before the closing bracket.
    CHECK(f.text().indexOf(QStringLiteral("htop"))
        < f.text().indexOf(QStringLiteral("    ];\n\n    fonts")));

    f.removePackage(QStringLiteral("environment.systemPackages"), QStringLiteral("htop"));
    CHECK_EQ(f.text(), src);

    f.setPackageEnabled(QStringLiteral("environment.systemPackages"), QStringLiteral("git"), false);
    CHECK(f.text().contains(QStringLiteral("        #git\n")));
    f.setPackageEnabled(QStringLiteral("environment.systemPackages"), QStringLiteral("git"), true);
    CHECK_EQ(f.text(), src);

    // Comments keep their trailing note across a disable/enable round trip.
    f.setPackageEnabled(QStringLiteral("environment.systemPackages"),
        QStringLiteral("uutils-coreutils-noprefix"), false);
    CHECK(f.text().contains(QStringLiteral("#uutils-coreutils-noprefix  # Rust coreutils")));
}

static void testAttributes()
{
    const QString src = R"NIX({ pkgs, ... }:

{
    networking.hostName = "main-pc";

    users.users.justin = {
        isNormalUser = true;
        description  = "Justin";
        extraGroups  = [ "wheel" "docker" ];
    };

    services.teamviewer.enable = true;
    nixos.pkgs.wallpaper-engine-kde-plugin.enable = true;
    system.stateVersion = "24.05";
}
)NIX";
    NixFile f = fromText(src);
    const AttrEntry *host = f.findAttr(QStringLiteral("networking.hostName"));
    CHECK(host != nullptr);
    if (host)
        CHECK_EQ(host->unquoted(), QStringLiteral("main-pc"));

    // Nested attrsets contribute their prefix.
    const AttrEntry *desc = f.findAttr(QStringLiteral("users.users.justin.description"));
    CHECK(desc != nullptr);
    if (desc)
        CHECK_EQ(desc->unquoted(), QStringLiteral("Justin"));
    CHECK(f.findAttr(QStringLiteral("users.users.justin.isNormalUser")) != nullptr);
    CHECK(f.findList(QStringLiteral("users.users.justin.extraGroups")) != nullptr);

    f.setAttribute(QStringLiteral("networking.hostName"), QStringLiteral("\"laptop\""));
    CHECK(f.text().contains(QStringLiteral("networking.hostName = \"laptop\";")));

    f.setAttribute(QStringLiteral("services.teamviewer.enable"), QStringLiteral("false"));
    CHECK(f.text().contains(QStringLiteral("services.teamviewer.enable = false;")));

    // A brand new attribute is appended just before the body's closing brace.
    f.setAttribute(QStringLiteral("services.openssh.enable"), QStringLiteral("true"));
    CHECK(f.text().contains(QStringLiteral("    services.openssh.enable = true;\n}")));

    f.removeAttribute(QStringLiteral("services.openssh.enable"));
    CHECK(!f.text().contains(QStringLiteral("services.openssh.enable")));
}

static void testOptionDeclarations()
{
    const QString src = R"NIX({ config, lib, pkgs, ... }:

let
    thing = pkgs.hello;
in
{
    options.nixos = {
        pkgs.wallpaper-engine-kde-plugin = {
            enable = lib.mkOption {
                type = lib.types.bool;
                default = false;
                example = true;
                description = "Enable wallpaper-engine-kde-plugin.";
            };
        };
    };

    config = lib.mkIf (config.nixos.pkgs.wallpaper-engine-kde-plugin.enable) {
        environment.systemPackages = with pkgs; [
            thing
        ];
    };
}
)NIX";
    NixFile f = fromText(src);
    CHECK_EQ(f.optionDecls().size(), qsizetype(1));
    if (!f.optionDecls().isEmpty()) {
        const OptionDecl &o = f.optionDecls().first();
        CHECK_EQ(o.path, QStringLiteral("nixos.pkgs.wallpaper-engine-kde-plugin.enable"));
        CHECK_EQ(o.defaultValue, QStringLiteral("false"));
        CHECK_EQ(o.description, QStringLiteral("Enable wallpaper-engine-kde-plugin."));
        CHECK(o.type.contains(QStringLiteral("bool")));
    }

    // The list hidden behind `lib.mkIf` is still found, with `config.` stripped.
    const NixList *l = f.findList(QStringLiteral("environment.systemPackages"));
    CHECK(l != nullptr);
    if (l)
        CHECK_EQ(l->entries.size(), qsizetype(1));
}

static void testAttrSetInsertion()
{
    // The alignment of the existing members has to be carried over.
    const QString src = R"NIX({
    outputs = { self, nixpkgs, ... }@inputs:
    let mkHost = hostModule: nixpkgs.lib.nixosSystem { modules = [ hostModule ]; };
    in {
        nixosConfigurations = {
            main-pc             = mkHost ./hosts/main-pc;
            t420                = mkHost ./hosts/t420;
        };
    };
}
)NIX";
    NixFile f = fromText(src);

    const NixAttrSet *set = f.findSet(QStringLiteral("outputs.nixosConfigurations"));
    CHECK(set != nullptr);

    CHECK(f.addToAttrSet(QStringLiteral("outputs.nixosConfigurations"),
        QStringLiteral("laptop"), QStringLiteral("mkHost ./hosts/laptop")));
    CHECK(f.text().contains(
        QStringLiteral("            laptop              = mkHost ./hosts/laptop;")));

    // It lands inside the set, after the last member.
    const int laptop = f.text().indexOf(QStringLiteral("laptop  "));
    const int closing = f.text().indexOf(QStringLiteral("        };"));
    CHECK(laptop > 0);
    CHECK(laptop < closing);
    CHECK(f.findAttr(QStringLiteral("outputs.nixosConfigurations.laptop")) != nullptr);

    // Adding the same name again updates the value rather than duplicating it.
    CHECK(f.addToAttrSet(QStringLiteral("outputs.nixosConfigurations"),
        QStringLiteral("laptop"), QStringLiteral("mkHost ./hosts/other")));
    CHECK(f.text().contains(QStringLiteral("mkHost ./hosts/other")));
    CHECK(!f.text().contains(QStringLiteral("mkHost ./hosts/laptop")));
    CHECK_EQ(f.text().count(QStringLiteral("laptop ")), 1);

    // An unknown set is reported rather than silently appended somewhere.
    CHECK(!f.addToAttrSet(QStringLiteral("nope.missing"), QStringLiteral("x"),
        QStringLiteral("1")));
}

static void testOptionEditing()
{
    const QString src = R"NIX({ pkgs, ... }:

{
    networking.hostName = "main-pc";
    services.teamviewer.enable = true;

    users.users.justin = {
        isNormalUser = true;
        description  = "Justin";
    };
}
)NIX";
    NixFile f = fromText(src);

    // Edit in place.
    CHECK(f.setAttribute(QStringLiteral("services.teamviewer.enable"),
        QStringLiteral("false")));
    CHECK(f.text().contains(QStringLiteral("services.teamviewer.enable = false;")));

    // Edit a value nested inside an attrset, without disturbing its siblings.
    CHECK(f.setAttribute(QStringLiteral("users.users.justin.description"),
        QStringLiteral("\"Justin R\"")));
    CHECK(f.text().contains(QStringLiteral("description  = \"Justin R\";")));
    CHECK(f.text().contains(QStringLiteral("isNormalUser = true;")));

    // Add a brand new one.
    CHECK(f.setAttribute(QStringLiteral("services.openssh.enable"), QStringLiteral("true")));
    CHECK(f.text().contains(QStringLiteral("    services.openssh.enable = true;\n}")));

    // Remove it again; the rest of the file is untouched.
    CHECK(f.removeAttribute(QStringLiteral("services.openssh.enable")));
    CHECK(!f.text().contains(QStringLiteral("openssh")));
    CHECK(f.text().contains(QStringLiteral("networking.hostName = \"main-pc\";")));

    // Removing a nested option leaves the enclosing set intact.
    CHECK(f.removeAttribute(QStringLiteral("users.users.justin.description")));
    CHECK(!f.text().contains(QStringLiteral("Justin R")));
    CHECK(f.findAttr(QStringLiteral("users.users.justin.isNormalUser")) != nullptr);
}

static void testFlakeHosts()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString root = dir.path();
    QDir(root).mkpath(QStringLiteral("hosts/main-pc"));
    QDir(root).mkpath(QStringLiteral("hosts/t420"));
    QDir(root).mkpath(QStringLiteral("modules/common"));
    QDir(root).mkpath(QStringLiteral("modules/desktop"));

    auto write = [](const QString &p, const QString &c) {
        QFile f(p);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream(&f) << c;
        }
    };

    write(root + QStringLiteral("/flake.nix"), R"NIX({
    description = "Nixos config flake";

    inputs = {
        nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
        nixpkgs-stable.url = "github:nixos/nixpkgs/nixos-25.05";
    };

    outputs = { self, nixpkgs, nixpkgs-stable, ... }@inputs:
    let mkHost = hostModule: nixpkgs.lib.nixosSystem {
            specialArgs = { inherit inputs; };
            modules = [
                hostModule
                ./plugins/wallpaper-engine-kde-plugin.nix
            ];
        };
    in {
        nixosConfigurations = {
            main-pc = mkHost ./hosts/main-pc;
            t420    = mkHost ./hosts/t420;
        };
    };
}
)NIX");
    write(root + QStringLiteral("/hosts/main-pc/default.nix"),
        QStringLiteral("{ ... }:\n{\n  networking.hostName = \"main-pc\";\n}\n"));
    write(root + QStringLiteral("/hosts/t420/default.nix"),
        QStringLiteral("{ ... }:\n{\n  networking.hostName = \"t420\";\n}\n"));
    write(root + QStringLiteral("/modules/common/nix.nix"), QStringLiteral("{ ... }:\n{\n}\n"));
    write(root + QStringLiteral("/modules/desktop/plasma6.nix"),
        QStringLiteral("{ pkgs, ... }:\n{\n  environment.systemPackages = with pkgs; [ vim ];\n}\n"));

    ConfigProject project;
    QString err;
    CHECK(project.open(root, &err));
    CHECK_EQ(project.kind() == ConfigProject::Flake, true);
    CHECK_EQ(project.hosts().size(), qsizetype(2));
    if (project.hosts().size() == 2) {
        CHECK_EQ(project.hosts().at(0).name, QStringLiteral("main-pc"));
        CHECK(project.hosts().at(0).entryFile.endsWith(QStringLiteral("hosts/main-pc/default.nix")));
    }
    CHECK_EQ(project.modules().size(), qsizetype(2));
    for (const ModuleInfo &m : project.modules())
        CHECK(!m.relPath.startsWith(QStringLiteral("hosts/")));
    CHECK_EQ(project.nixpkgsChannel(), QStringLiteral("nixos-unstable"));
    CHECK_EQ(project.flakeInputs().size(), qsizetype(2));
}

static void testSingleFileProject()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString root = dir.path();
    QFile f(root + QStringLiteral("/configuration.nix"));
    CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&f) << R"NIX({ config, pkgs, ... }:
{
  imports = [ ./hardware-configuration.nix ];
  networking.hostName = "nixos";
  environment.systemPackages = with pkgs; [ vim wget ];
  system.stateVersion = "24.05";
}
)NIX";
    f.close();

    ConfigProject project;
    QString err;
    CHECK(project.open(root, &err));
    CHECK_EQ(project.kind() == ConfigProject::SingleFile, true);
    CHECK_EQ(project.hosts().size(), qsizetype(1));
    if (!project.hosts().isEmpty())
        CHECK_EQ(project.hosts().at(0).name, QStringLiteral("nixos"));

    // The entry file must not be offered as a module of itself: importing
    // ./configuration.nix into configuration.nix is infinite recursion.
    for (const ModuleInfo &m : project.modules())
        CHECK(!m.absPath.endsWith(QStringLiteral("configuration.nix")));

    // A real module alongside it still shows up.
    QFile extra(root + QStringLiteral("/desktop.nix"));
    CHECK(extra.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&extra) << QStringLiteral("{ ... }:\n{\n}\n");
    extra.close();
    project.rescan();
    CHECK_EQ(project.modules().size(), qsizetype(1));
    if (!project.modules().isEmpty())
        CHECK_EQ(project.modules().at(0).name, QStringLiteral("desktop"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testLexer();
    testImports();
    testPackages();
    testAttributes();
    testOptionDeclarations();
    testAttrSetInsertion();
    testOptionEditing();
    testFlakeHosts();
    testSingleFileProject();

    QTextStream(stdout) << (g_failures == 0 ? "PASS" : "FAIL") << ": " << (g_checks - g_failures)
                        << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
