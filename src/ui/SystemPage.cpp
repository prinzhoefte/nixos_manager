#include "SystemPage.h"

#include "core/CommandRunner.h"
#include "core/ConfigProject.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace nixm {

SystemPage::SystemPage(const AppContext &ctx, QWidget *parent)
    : QWidget(parent)
    , m_ctx(ctx)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);

    layout->addWidget(buildRebuildGroup());
    layout->addWidget(buildGenerationsGroup());
    layout->addWidget(buildFlakeGroup());
    layout->addWidget(buildCleanupGroup());
    layout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll);

    connect(m_ctx.runner, &CommandRunner::runningChanged, this,
        [this](bool running) { setControlsEnabled(!running); });
    connect(m_ctx.runner, &CommandRunner::batchFinished, this, [this](bool ok) {
        if (ok)
            refreshGenerations();
    });
}

QWidget *SystemPage::buildRebuildGroup()
{
    auto *group = new QGroupBox(tr("Rebuild"), this);
    auto *layout = new QVBoxLayout(group);
    auto *form = new QFormLayout;

    m_hostBox = new QComboBox(group);
    form->addRow(tr("Host:"), m_hostBox);

    m_actionBox = new QComboBox(group);
    for (auto action : SystemOps::allRebuildActions()) {
        m_actionBox->addItem(SystemOps::rebuildActionName(action), int(action));
    }
    m_actionBox->setCurrentIndex(m_actionBox->findText(QStringLiteral("boot")));
    form->addRow(tr("Action:"), m_actionBox);

    m_extraArgs = new QLineEdit(group);
    m_extraArgs->setPlaceholderText(QStringLiteral("--show-trace  --option substituters …"));
    form->addRow(tr("Extra arguments:"), m_extraArgs);
    layout->addLayout(form);

    m_saveFirst = new QCheckBox(tr("Save modified files before rebuilding"), group);
    m_saveFirst->setChecked(true);
    layout->addWidget(m_saveFirst);

    auto *hint = new QLabel(group);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    hint->setText(tr("switch activates now and on boot · boot activates on next boot · test "
                     "activates now without touching the bootloader · dry-build only evaluates."));
    layout->addWidget(hint);

    auto *row = new QHBoxLayout;
    m_rebuildButton = new QPushButton(tr("Run rebuild"), group);
    connect(m_rebuildButton, &QPushButton::clicked, this, &SystemPage::runRebuild);
    row->addWidget(m_rebuildButton);
    row->addStretch(1);
    layout->addLayout(row);

    return group;
}

QWidget *SystemPage::buildGenerationsGroup()
{
    auto *group = new QGroupBox(tr("System generations"), this);
    auto *layout = new QVBoxLayout(group);

    m_generations = new QTableWidget(group);
    m_generations->setColumnCount(4);
    m_generations->setHorizontalHeaderLabels(
        { tr("Generation"), tr("Built"), tr("NixOS version"), tr("Kernel") });
    m_generations->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_generations->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_generations->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_generations->verticalHeader()->setVisible(false);
    m_generations->horizontalHeader()->setStretchLastSection(true);
    m_generations->setAlternatingRowColors(true);
    m_generations->setMinimumHeight(180);
    layout->addWidget(m_generations);

    auto *row = new QHBoxLayout;
    auto *refreshButton = new QPushButton(tr("Refresh"), group);
    connect(refreshButton, &QPushButton::clicked, this, &SystemPage::refreshGenerations);
    row->addWidget(refreshButton);

    m_rollbackButton = new QPushButton(tr("Roll back to selected"), group);
    connect(m_rollbackButton, &QPushButton::clicked, this, &SystemPage::runRollback);
    row->addWidget(m_rollbackButton);

    m_deleteGenButton = new QPushButton(tr("Delete selected"), group);
    connect(m_deleteGenButton, &QPushButton::clicked, this, &SystemPage::runDeleteGenerations);
    row->addWidget(m_deleteGenButton);
    row->addStretch(1);
    layout->addLayout(row);

    return group;
}

QWidget *SystemPage::buildFlakeGroup()
{
    auto *group = new QGroupBox(tr("Flake inputs"), this);
    auto *layout = new QVBoxLayout(group);

    m_inputs = new QTableWidget(group);
    m_inputs->setColumnCount(4);
    m_inputs->setHorizontalHeaderLabels(
        { tr("Input"), tr("URL"), tr("Locked revision"), tr("Last modified") });
    m_inputs->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_inputs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_inputs->verticalHeader()->setVisible(false);
    m_inputs->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_inputs->setAlternatingRowColors(true);
    m_inputs->setMaximumHeight(160);
    layout->addWidget(m_inputs);

    auto *row = new QHBoxLayout;
    m_updateAll = new QPushButton(tr("Update all inputs"), group);
    connect(m_updateAll, &QPushButton::clicked, this, [this] { runFlakeUpdate(false); });
    row->addWidget(m_updateAll);

    m_updateSelected = new QPushButton(tr("Update selected input"), group);
    connect(m_updateSelected, &QPushButton::clicked, this, [this] { runFlakeUpdate(true); });
    row->addWidget(m_updateSelected);

    m_flakeAsRoot = new QCheckBox(tr("Run as root"), group);
    m_flakeAsRoot->setToolTip(
        tr("Needed when the configuration lives somewhere only root can write, such as "
           "/etc/nixos."));
    row->addWidget(m_flakeAsRoot);
    row->addStretch(1);
    layout->addLayout(row);

    return group;
}

QWidget *SystemPage::buildCleanupGroup()
{
    auto *group = new QGroupBox(tr("Cleanup"), this);
    auto *layout = new QVBoxLayout(group);

    auto *hint = new QLabel(
        tr("Frees disk space by dropping old generations and collecting store garbage — the same "
           "thing as running <code>nix-env --delete-generations old &amp;&amp; "
           "nix-collect-garbage -d</code> by hand."),
        group);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    m_cleanSystemGenerations
        = new QCheckBox(tr("Delete old system generations"), group);
    m_cleanSystemGenerations->setChecked(true);
    layout->addWidget(m_cleanSystemGenerations);

    m_cleanUserProfiles = new QCheckBox(tr("Delete old user profile generations"), group);
    m_cleanUserProfiles->setChecked(true);
    layout->addWidget(m_cleanUserProfiles);

    m_cleanCollectGarbage
        = new QCheckBox(tr("Collect garbage (delete unreachable store paths)"), group);
    m_cleanCollectGarbage->setChecked(true);
    layout->addWidget(m_cleanCollectGarbage);

    m_cleanOptimise = new QCheckBox(tr("Optimise the store (hard-link identical files)"), group);
    layout->addWidget(m_cleanOptimise);

    auto *daysRow = new QHBoxLayout;
    daysRow->addWidget(new QLabel(tr("Keep generations newer than:"), group));
    m_cleanDays = new QSpinBox(group);
    m_cleanDays->setRange(0, 365);
    m_cleanDays->setValue(7);
    m_cleanDays->setSuffix(tr(" days"));
    m_cleanDays->setSpecialValueText(tr("keep only the current one"));
    daysRow->addWidget(m_cleanDays);
    daysRow->addStretch(1);
    layout->addLayout(daysRow);

    m_cleanDryRun = new QCheckBox(tr("Dry run — show what would be deleted"), group);
    m_cleanDryRun->setChecked(true);
    layout->addWidget(m_cleanDryRun);

    auto *row = new QHBoxLayout;
    m_cleanupButton = new QPushButton(tr("Run cleanup"), group);
    connect(m_cleanupButton, &QPushButton::clicked, this, &SystemPage::runCleanup);
    row->addWidget(m_cleanupButton);

    m_storeSize = new QLabel(group);
    row->addWidget(m_storeSize);

    auto *measure = new QPushButton(tr("Measure /nix/store"), group);
    connect(measure, &QPushButton::clicked, this, [this] {
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        const QString size = SystemOps::storeSize();
        QGuiApplication::restoreOverrideCursor();
        m_storeSize->setText(size.isEmpty() ? tr("(unavailable)")
                                            : tr("/nix/store is %1").arg(size));
    });
    row->addWidget(measure);
    row->addStretch(1);
    layout->addLayout(row);

    return group;
}

void SystemPage::refresh()
{
    const QString previous = m_hostBox->currentText();
    m_hostBox->clear();
    if (m_ctx.project && m_ctx.project->isOpen()) {
        for (const HostInfo &h : m_ctx.project->hosts())
            m_hostBox->addItem(h.name);
        const int index = m_hostBox->findText(previous);
        if (index >= 0)
            m_hostBox->setCurrentIndex(index);
        m_flakeAsRoot->setChecked(!m_ctx.project->isWritable());

        const bool isFlake = m_ctx.project->kind() == ConfigProject::Flake;
        m_updateAll->setEnabled(isFlake);
        m_updateSelected->setEnabled(isFlake);
        m_inputs->setEnabled(isFlake);
        m_hostBox->setEnabled(isFlake);
    }
    refreshFlakeInputs();
    refreshGenerations();
}

void SystemPage::refreshGenerations()
{
    const auto generations = SystemOps::listGenerations();
    m_generations->clearSpans();
    m_generations->setRowCount(generations.size());
    for (int row = 0; row < generations.size(); ++row) {
        const Generation &g = generations.at(row);
        auto *number = new QTableWidgetItem(
            g.current ? tr("%1  (current)").arg(g.number) : QString::number(g.number));
        number->setData(Qt::UserRole, g.number);
        if (g.current) {
            QFont f = number->font();
            f.setBold(true);
            number->setFont(f);
        }
        m_generations->setItem(row, 0, number);
        m_generations->setItem(row, 1, new QTableWidgetItem(g.date));
        m_generations->setItem(row, 2, new QTableWidgetItem(g.nixosVersion));
        m_generations->setItem(row, 3, new QTableWidgetItem(g.kernel));
    }
    m_generations->resizeColumnsToContents();

    const bool any = !generations.isEmpty();
    m_rollbackButton->setEnabled(any);
    m_deleteGenButton->setEnabled(any);
    if (!any) {
        m_generations->setRowCount(1);
        auto *item = new QTableWidgetItem(
            tr("No system profile found — is this machine running NixOS?"));
        item->setFlags(Qt::NoItemFlags);
        m_generations->setItem(0, 0, item);
        m_generations->setSpan(0, 0, 1, 4);
    }
}

void SystemPage::refreshFlakeInputs()
{
    m_inputs->setRowCount(0);
    if (!m_ctx.project || m_ctx.project->kind() != ConfigProject::Flake)
        return;

    const auto locked = SystemOps::lockedInputs(m_ctx.project->root());
    const auto inputs = m_ctx.project->flakeInputs();
    m_inputs->setRowCount(inputs.size());
    for (int row = 0; row < inputs.size(); ++row) {
        const FlakeInput &in = inputs.at(row);
        m_inputs->setItem(row, 0, new QTableWidgetItem(in.name));
        m_inputs->setItem(row, 1, new QTableWidgetItem(in.url));
        const auto lock = locked.value(in.name);
        m_inputs->setItem(row, 2, new QTableWidgetItem(lock.first));
        m_inputs->setItem(row, 3, new QTableWidgetItem(lock.second));
    }
    m_inputs->resizeColumnToContents(0);
}

void SystemPage::startRebuildFor(const QString &host)
{
    const int index = m_hostBox->findText(host);
    if (index >= 0)
        m_hostBox->setCurrentIndex(index);
}

void SystemPage::setControlsEnabled(bool enabled)
{
    m_rebuildButton->setEnabled(enabled);
    m_rollbackButton->setEnabled(enabled);
    m_deleteGenButton->setEnabled(enabled);
    m_updateAll->setEnabled(enabled && m_ctx.project
        && m_ctx.project->kind() == ConfigProject::Flake);
    m_updateSelected->setEnabled(enabled && m_ctx.project
        && m_ctx.project->kind() == ConfigProject::Flake);
    m_cleanupButton->setEnabled(enabled);
}

void SystemPage::runRebuild()
{
    if (!m_ctx.project || !m_ctx.project->isOpen())
        return;

    if (m_saveFirst->isChecked() && m_ctx.project->hasUnsavedChanges())
        emit saveRequested();

    const auto action
        = SystemOps::RebuildAction(m_actionBox->currentData().toInt());
    const QStringList extra = m_extraArgs->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const bool isFlake = m_ctx.project->kind() == ConfigProject::Flake;

    const auto step = SystemOps::rebuild(action, m_ctx.project->root(),
        isFlake ? m_hostBox->currentText() : QString(), isFlake, extra);

    if (SystemOps::rebuildNeedsRoot(action)) {
        const auto answer = QMessageBox::question(this, tr("Rebuild"),
            tr("Run this now?\n\n%1\n\nYou will be asked for your password.")
                .arg(m_ctx.runner->commandLine(step)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return;
    }

    m_ctx.runner->run(step);
}

QVector<int> SystemPage::selectedGenerations() const
{
    QVector<int> out;
    const auto rows = m_generations->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        auto *item = m_generations->item(index.row(), 0);
        if (!item)
            continue;
        const int number = item->data(Qt::UserRole).toInt();
        if (number > 0)
            out.push_back(number);
    }
    return out;
}

void SystemPage::runRollback()
{
    const auto selected = selectedGenerations();
    if (selected.size() != 1) {
        QMessageBox::information(this, tr("Roll back"),
            tr("Select exactly one generation to roll back to."));
        return;
    }

    const int generation = selected.first();
    const auto answer = QMessageBox::question(this, tr("Roll back"),
        tr("Switch the running system to generation %1?\n\nThis activates the old configuration "
           "immediately and makes it the default at next boot.")
            .arg(generation),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_ctx.runner->run(SystemOps::rollbackTo(generation));
}

void SystemPage::runDeleteGenerations()
{
    const auto selected = selectedGenerations();
    if (selected.isEmpty())
        return;

    QStringList numbers;
    for (int g : selected)
        numbers << QString::number(g);

    const auto answer = QMessageBox::question(this, tr("Delete generations"),
        tr("Permanently delete generation(s) %1?\n\nThe store paths are only freed once you also "
           "collect garbage.")
            .arg(numbers.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    m_ctx.runner->run(SystemOps::deleteGenerations(selected));
}

void SystemPage::runFlakeUpdate(bool selectedOnly)
{
    if (!m_ctx.project || m_ctx.project->kind() != ConfigProject::Flake)
        return;

    const bool asRoot = m_flakeAsRoot->isChecked();
    if (!selectedOnly) {
        m_ctx.runner->run(SystemOps::flakeUpdateAll(m_ctx.project->root(), asRoot));
        return;
    }

    const auto rows = m_inputs->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Update input"), tr("Select an input first."));
        return;
    }
    QVector<CommandRunner::Step> steps;
    for (const QModelIndex &index : rows) {
        auto *item = m_inputs->item(index.row(), 0);
        if (item)
            steps << SystemOps::flakeUpdateInput(m_ctx.project->root(), item->text(), asRoot);
    }
    m_ctx.runner->run(steps);
}

void SystemPage::runCleanup()
{
    SystemOps::CleanupOptions opts;
    opts.deleteSystemGenerations = m_cleanSystemGenerations->isChecked();
    opts.userProfiles = m_cleanUserProfiles->isChecked();
    opts.collectGarbage = m_cleanCollectGarbage->isChecked();
    opts.optimiseStore = m_cleanOptimise->isChecked();
    opts.olderThanDays = m_cleanDays->value();
    opts.dryRun = m_cleanDryRun->isChecked();

    const auto steps = SystemOps::cleanup(opts);
    if (steps.isEmpty()) {
        QMessageBox::information(this, tr("Cleanup"), tr("Nothing selected."));
        return;
    }

    if (!opts.dryRun) {
        QStringList lines;
        for (const auto &step : steps)
            lines << QStringLiteral("  ") + m_ctx.runner->commandLine(step);
        const auto answer = QMessageBox::question(this, tr("Cleanup"),
            tr("This will run:\n\n%1\n\nDeleted generations cannot be recovered. Continue?")
                .arg(lines.join(QLatin1Char('\n'))),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    m_ctx.runner->run(steps);
}

} // namespace nixm
