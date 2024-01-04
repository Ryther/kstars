/*
    SPDX-FileCopyrightText: 2015 Jasem Mutlaq <mutlaqja@ikarustech.com>

    DBus calls from GSoC 2015 Ekos Scheduler project:
    SPDX-FileCopyrightText: 2015 Daniel Leu <daniel_mihai.leu@cti.pub.ro>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scheduler.h"

#include "ekos/scheduler/framingassistantui.h"
#include "ksnotification.h"
#include "ksmessagebox.h"
#include "kstars.h"
#include "kstarsdata.h"
#include "skymap.h"
#include "Options.h"
#include "scheduleradaptor.h"
#include "schedulerjob.h"
#include "schedulerprocess.h"
#include "schedulermodulestate.h"
#include "schedulerutils.h"
#include "skymapcomposite.h"
#include "skycomponents/mosaiccomponent.h"
#include "skyobjects/mosaictiles.h"
#include "auxiliary/QProgressIndicator.h"
#include "dialogs/finddialog.h"
#include "ekos/manager.h"
#include "ekos/capture/sequencejob.h"
#include "ekos/capture/placeholderpath.h"
#include "skyobjects/starobject.h"
#include "greedyscheduler.h"
#include "ekos/auxiliary/solverutils.h"
#include "ekos/auxiliary/stellarsolverprofile.h"

#include <KConfigDialog>
#include <KActionCollection>

#include <fitsio.h>
#include <ekos_scheduler_debug.h>
#include <indicom.h>

// Qt version calming
#include <qtendl.h>

#define BAD_SCORE                -1000
#define RESTART_GUIDING_DELAY_MS  5000

#define DEFAULT_MIN_ALTITUDE        15
#define DEFAULT_MIN_MOON_SEPARATION 0

// This is a temporary debugging printout introduced while gaining experience developing
// the unit tests in test_ekos_scheduler_ops.cpp.
// All these printouts should be eventually removed.
#define TEST_PRINT if (false) fprintf

namespace
{

// This needs to match the definition order for the QueueTable in scheduler.ui
enum QueueTableColumns
{
    NAME_COLUMN = 0,
    STATUS_COLUMN,
    CAPTURES_COLUMN,
    ALTITUDE_COLUMN,
    START_TIME_COLUMN,
    END_TIME_COLUMN,
};
}

namespace Ekos
{

// Functions to make human-readable debug messages for the various enums.
namespace
{
QString commStatusString(Ekos::CommunicationStatus state)
{
    switch(state)
    {
        case Ekos::Idle:
            return "Idle";
        case Ekos::Pending:
            return "Pending";
        case Ekos::Success:
            return "Success";
        case Ekos::Error:
            return "Error";
    }
    return QString("????");
}

QString schedulerStateString(Ekos::SchedulerState state)
{
    switch(state)
    {
        case Ekos::SCHEDULER_IDLE:
            return "SCHEDULER_IDLE";
        case Ekos::SCHEDULER_STARTUP:
            return "SCHEDULER_STARTUP";
        case Ekos::SCHEDULER_RUNNING:
            return "SCHEDULER_RUNNING";
        case Ekos::SCHEDULER_PAUSED:
            return "SCHEDULER_PAUSED";
        case Ekos::SCHEDULER_SHUTDOWN:
            return "SCHEDULER_SHUTDOWN";
        case Ekos::SCHEDULER_ABORTED:
            return "SCHEDULER_ABORTED";
        case Ekos::SCHEDULER_LOADING:
            return "SCHEDULER_LOADING";
    }
    return QString("????");
}

void printJobsStatus(const QList<SchedulerJob *> &jobs)
{
    foreach (auto j, jobs)
        TEST_PRINT(stderr, "job %s", QString("%1 %2\n").arg(j->getName())
                   .arg(SchedulerJob::jobStatusString(j->getState())).toLatin1().data());
}

}  // namespace

void Scheduler::printStates(const QString &label)
{
    TEST_PRINT(stderr, "%s",
               QString("%1 %2 %3%4 %5 %6 %7 %8 %9\n")
               .arg(label)
               .arg(timerStr(moduleState()->timerState()))
               .arg(schedulerStateString(moduleState()->schedulerState()))
               .arg((moduleState()->timerState() == RUN_JOBCHECK && activeJob() != nullptr) ?
                    QString("(%1 %2)").arg(SchedulerJob::jobStatusString(activeJob()->getState()))
                    .arg(SchedulerJob::jobStageString(activeJob()->getStage())) : "")
               .arg(ekosStateString(moduleState()->ekosState()))
               .arg(indiStateString(moduleState()->indiState()))
               .arg(startupStateString(moduleState()->startupState()))
               .arg(shutdownStateString(moduleState()->shutdownState()))
               .arg(parkWaitStateString(moduleState()->parkWaitState())).toLatin1().data());
    printJobsStatus(moduleState()->jobs());
}


// This is the initial conditions that need to be set before starting.
void Scheduler::init()
{
    // This is needed to get wakeupScheduler() to call start() and startup,
    // instead of assuming it is already initialized (if preemptiveShutdown was not set).
    // The time itself is not used.
    moduleState()->enablePreemptiveShutdown(SchedulerModuleState::getLocalTime());

    moduleState()->setIterationSetup(false);
    moduleState()->setupNextIteration(RUN_WAKEUP, 10);
}

// Setup the main loop and start.
void Scheduler::start()
{

    // New scheduler session shouldn't inherit ABORT or ERROR states from the last one.
    foreach (auto j, moduleState()->jobs())
    {
        j->setState(SCHEDJOB_IDLE);
        updateJobTable(j);
    }
    init();
    iterate();
}

// This is the main scheduler loop.
// Run an iteration, get the sleep time, sleep for that interval, and repeat.
void Scheduler::iterate()
{
    const int msSleep = runSchedulerIteration();
    if (msSleep < 0)
        return;

    connect(&moduleState()->iterationTimer(), &QTimer::timeout, this, &Scheduler::iterate, Qt::UniqueConnection);
    moduleState()->iterationTimer().setSingleShot(true);
    moduleState()->iterationTimer().start(msSleep);
}

bool Scheduler::currentlySleeping()
{
    return moduleState()->iterationTimer().isActive() && moduleState()->timerState() == RUN_WAKEUP;
}

int Scheduler::runSchedulerIteration()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (moduleState()->startMSecs() == 0)
        moduleState()->setStartMSecs(now);

    printStates(QString("\nrunScheduler Iteration %1 @ %2")
                .arg(moduleState()->increaseSchedulerIteration())
                .arg((now - moduleState()->startMSecs()) / 1000.0, 1, 'f', 3));

    SchedulerTimerState keepTimerState = moduleState()->timerState();

    // TODO: At some point we should require that timerState and timerInterval
    // be explicitly set in all iterations. Not there yet, would require too much
    // refactoring of the scheduler. When we get there, we'd exectute the following here:
    // timerState = RUN_NOTHING;    // don't like this comment, it should always set a state and interval!
    // timerInterval = -1;
    moduleState()->setIterationSetup(false);
    switch (keepTimerState)
    {
        case RUN_WAKEUP:
            wakeUpScheduler();
            break;
        case RUN_SCHEDULER:
            checkStatus();
            break;
        case RUN_JOBCHECK:
            checkJobStage();
            break;
        case RUN_SHUTDOWN:
            checkShutdownState();
            break;
        case RUN_NOTHING:
            moduleState()->setTimerInterval(-1);
            break;
    }
    if (!moduleState()->iterationSetup())
    {
        // See the above TODO.
        // Since iterations aren't yet always set up, we repeat the current
        // iteration type if one wasn't set up in the current iteration.
        // qCDebug(KSTARS_EKOS_SCHEDULER) << "Scheduler iteration never set up.";
        moduleState()->setTimerInterval(moduleState()->updatePeriodMs());
        TEST_PRINT(stderr, "Scheduler iteration never set up--repeating %s with %d...\n",
                   timerStr(moduleState()->timerState()).toLatin1().data(), moduleState()->timerInterval());
    }
    printStates(QString("End iteration, sleep %1: ").arg(moduleState()->timerInterval()));
    return moduleState()->timerInterval();
}

Scheduler::Scheduler()
{
    // Use the default path and interface when running the scheduler.
    setupScheduler(ekosPathString, ekosInterfaceString);
}

Scheduler::Scheduler(const QString path, const QString interface,
                     const QString &ekosPathStr, const QString &ekosInterfaceStr)
{
    // During testing, when mocking ekos, use a special purpose path and interface.
    schedulerPathString = path;
    kstarsInterfaceString = interface;
    setupScheduler(ekosPathStr, ekosInterfaceStr);
}

void Scheduler::setupScheduler(const QString &ekosPathStr, const QString &ekosInterfaceStr)
{
    setupUi(this);

    qRegisterMetaType<Ekos::SchedulerState>("Ekos::SchedulerState");
    qDBusRegisterMetaType<Ekos::SchedulerState>();

    m_moduleState.reset(new SchedulerModuleState());
    m_process.reset(new SchedulerProcess(moduleState()));

    dirPath = QUrl::fromLocalFile(QDir::homePath());

    // Get current KStars time and set seconds to zero
    QDateTime currentDateTime = SchedulerModuleState::getLocalTime();
    QTime currentTime         = currentDateTime.time();
    currentTime.setHMS(currentTime.hour(), currentTime.minute(), 0);
    currentDateTime.setTime(currentTime);

    // Set initial time for startup and completion times
    startupTimeEdit->setDateTime(currentDateTime);
    schedulerUntilValue->setDateTime(currentDateTime);

    m_GreedyScheduler = new GreedyScheduler();

    // Set up DBus interfaces
    new SchedulerAdaptor(this);
    QDBusConnection::sessionBus().unregisterObject(schedulerPathString);
    if (!QDBusConnection::sessionBus().registerObject(schedulerPathString, this))
        qCDebug(KSTARS_EKOS_SCHEDULER) << QString("Scheduler failed to register with dbus");
    process()->setEkosInterface(new QDBusInterface(kstarsInterfaceString, ekosPathStr, ekosInterfaceStr,
                                QDBusConnection::sessionBus(), this));

    process()->setIndiInterface(new QDBusInterface(kstarsInterfaceString, INDIPathString, INDIInterfaceString,
                                QDBusConnection::sessionBus(), this));

    // Example of connecting DBus signals
    //connect(ekosInterface, SIGNAL(indiStatusChanged(Ekos::CommunicationStatus)), this, SLOT(setINDICommunicationStatus(Ekos::CommunicationStatus)));
    //connect(ekosInterface, SIGNAL(ekosStatusChanged(Ekos::CommunicationStatus)), this, SLOT(setEkosCommunicationStatus(Ekos::CommunicationStatus)));
    //connect(ekosInterface, SIGNAL(newModule(QString)), this, SLOT(registerNewModule(QString)));
    QDBusConnection::sessionBus().connect(kstarsInterfaceString, ekosPathStr, ekosInterfaceStr, "newModule", this,
                                          SLOT(registerNewModule(QString)));
    QDBusConnection::sessionBus().connect(kstarsInterfaceString, ekosPathStr, ekosInterfaceStr, "newDevice", this,
                                          SLOT(registerNewDevice(QString, int)));
    QDBusConnection::sessionBus().connect(kstarsInterfaceString, ekosPathStr, ekosInterfaceStr, "indiStatusChanged",
                                          this, SLOT(setINDICommunicationStatus(Ekos::CommunicationStatus)));
    QDBusConnection::sessionBus().connect(kstarsInterfaceString, ekosPathStr, ekosInterfaceStr, "ekosStatusChanged",
                                          this, SLOT(setEkosCommunicationStatus(Ekos::CommunicationStatus)));

    sleepLabel->setPixmap(
        QIcon::fromTheme("chronometer").pixmap(QSize(32, 32)));
    sleepLabel->hide();

    pi = new QProgressIndicator(this);
    bottomLayout->addWidget(pi, 0);

    geo = KStarsData::Instance()->geo();

    //RA box should be HMS-style
    raBox->setUnits(dmsBox::HOURS);

    /* FIXME: Find a way to have multi-line tooltips in the .ui file, then move the widget configuration there - what about i18n? */

    queueTable->setToolTip(
        i18n("Job scheduler list.\nClick to select a job in the list.\nDouble click to edit a job with the left-hand fields."));
    QTableWidgetItem *statusHeader       = queueTable->horizontalHeaderItem(SCHEDCOL_STATUS);
    QTableWidgetItem *altitudeHeader     = queueTable->horizontalHeaderItem(SCHEDCOL_ALTITUDE);
    QTableWidgetItem *startupHeader      = queueTable->horizontalHeaderItem(SCHEDCOL_STARTTIME);
    QTableWidgetItem *completionHeader   = queueTable->horizontalHeaderItem(SCHEDCOL_ENDTIME);
    QTableWidgetItem *captureCountHeader = queueTable->horizontalHeaderItem(SCHEDCOL_CAPTURES);

    if (statusHeader != nullptr)
        statusHeader->setToolTip(i18n("Current status of the job, managed by the Scheduler.\n"
                                      "If invalid, the Scheduler was not able to find a proper observation time for the target.\n"
                                      "If aborted, the Scheduler missed the scheduled time or encountered transitory issues and will reschedule the job.\n"
                                      "If complete, the Scheduler verified that all sequence captures requested were stored, including repeats."));
    if (altitudeHeader != nullptr)
        altitudeHeader->setToolTip(i18n("Current altitude of the target of the job.\n"
                                        "A rising target is indicated with an arrow going up.\n"
                                        "A setting target is indicated with an arrow going down."));
    if (startupHeader != nullptr)
        startupHeader->setToolTip(i18n("Startup time of the job, as estimated by the Scheduler.\n"
                                       "The altitude at startup, if available, is displayed too.\n"
                                       "Fixed time from user or culmination time is marked with a chronometer symbol."));
    if (completionHeader != nullptr)
        completionHeader->setToolTip(i18n("Completion time for the job', as estimated by the Scheduler.\n"
                                          "You may specify a fixed time to limit duration of looping jobs. "
                                          "A warning symbol indicates the altitude at completion may cause the job to abort before completion.\n"));
    if (captureCountHeader != nullptr)
        captureCountHeader->setToolTip(i18n("Count of captures stored for the job, based on its sequence job.\n"
                                            "This is a summary, additional specific frame types may be required to complete the job."));

    /* Set first button mode to add observation job from left-hand fields */
    setJobAddApply(true);

    removeFromQueueB->setIcon(QIcon::fromTheme("list-remove"));
    removeFromQueueB->setToolTip(
        i18n("Remove selected job from the observation list.\nJob properties are copied in the edition fields before removal."));
    removeFromQueueB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    queueUpB->setIcon(QIcon::fromTheme("go-up"));
    queueUpB->setToolTip(i18n("Move selected job one line up in the list.\n"));
    queueUpB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    queueDownB->setIcon(QIcon::fromTheme("go-down"));
    queueDownB->setToolTip(i18n("Move selected job one line down in the list.\n"));
    queueDownB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    evaluateOnlyB->setIcon(QIcon::fromTheme("system-reboot"));
    evaluateOnlyB->setToolTip(i18n("Reset state and force reevaluation of all observation jobs."));
    evaluateOnlyB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    sortJobsB->setIcon(QIcon::fromTheme("transform-move-vertical"));
    sortJobsB->setToolTip(
        i18n("Reset state and sort observation jobs per altitude and movement in sky, using the start time of the first job.\n"
             "This action sorts setting targets before rising targets, and may help scheduling when starting your observation.\n"
             "Note the algorithm first calculates all altitudes using the same time, then evaluates jobs."));
    sortJobsB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    mosaicB->setIcon(QIcon::fromTheme("zoom-draw"));
    mosaicB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    positionAngleSpin->setSpecialValueText("--");

    queueSaveAsB->setIcon(QIcon::fromTheme("document-save-as"));
    queueSaveAsB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    queueSaveB->setIcon(QIcon::fromTheme("document-save"));
    queueSaveB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    queueLoadB->setIcon(QIcon::fromTheme("document-open"));
    queueLoadB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    queueAppendB->setIcon(QIcon::fromTheme("document-import"));
    queueAppendB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    loadSequenceB->setIcon(QIcon::fromTheme("document-open"));
    loadSequenceB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    selectStartupScriptB->setIcon(QIcon::fromTheme("document-open"));
    selectStartupScriptB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    selectShutdownScriptB->setIcon(
        QIcon::fromTheme("document-open"));
    selectShutdownScriptB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    selectFITSB->setIcon(QIcon::fromTheme("document-open"));
    selectFITSB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    startupB->setIcon(
        QIcon::fromTheme("media-playback-start"));
    startupB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    shutdownB->setIcon(
        QIcon::fromTheme("media-playback-start"));
    shutdownB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    // 2023-06-27 sterne-jaeger: For simplicity reasons, the repeat option
    // for all sequences is only active of we do consider the past
    repeatSequenceCB->setEnabled(Options::rememberJobProgress() == false);
    executionSequenceLimit->setEnabled(Options::rememberJobProgress() == false);
    repeatSequenceCB->setChecked(Options::schedulerRepeatSequences());
    executionSequenceLimit->setValue(Options::schedulerExecutionSequencesLimit());

    connect(startupB, &QPushButton::clicked, process().data(), &SchedulerProcess::runStartupProcedure);
    connect(shutdownB, &QPushButton::clicked, process().data(), &SchedulerProcess::runShutdownProcedure);

    connect(selectObjectB, &QPushButton::clicked, this, &Scheduler::selectObject);
    connect(selectFITSB, &QPushButton::clicked, this, &Scheduler::selectFITS);
    connect(loadSequenceB, &QPushButton::clicked, this, &Scheduler::selectSequence);
    connect(selectStartupScriptB, &QPushButton::clicked, this, &Scheduler::selectStartupScript);
    connect(selectShutdownScriptB, &QPushButton::clicked, this, &Scheduler::selectShutdownScript);

    connect(KStars::Instance()->actionCollection()->action("show_mosaic_panel"), &QAction::triggered, this, [this](bool checked)
    {
        mosaicB->setDown(checked);
    });
    connect(mosaicB, &QPushButton::clicked, this, []()
    {
        KStars::Instance()->actionCollection()->action("show_mosaic_panel")->trigger();
    });
    connect(addToQueueB, &QPushButton::clicked, [this]()
    {
        // add job from UI
        addJob();
    });
    connect(removeFromQueueB, &QPushButton::clicked, this, &Scheduler::removeJob);
    connect(queueUpB, &QPushButton::clicked, this, &Scheduler::moveJobUp);
    connect(queueDownB, &QPushButton::clicked, this, &Scheduler::moveJobDown);
    connect(evaluateOnlyB, &QPushButton::clicked, this, &Scheduler::startJobEvaluation);
    connect(sortJobsB, &QPushButton::clicked, this, &Scheduler::sortJobsPerAltitude);
    connect(queueTable->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &Scheduler::queueTableSelectionChanged);
    connect(queueTable, &QAbstractItemView::clicked, this, &Scheduler::clickQueueTable);
    connect(queueTable, &QAbstractItemView::doubleClicked, this, &Scheduler::loadJob);


    // These connections are looking for changes in the rows queueTable is displaying.
    connect(queueTable->verticalScrollBar(), &QScrollBar::valueChanged, [this](){updateJobTable();});
    connect(queueTable->verticalScrollBar(), &QAbstractSlider::rangeChanged, [this](){updateJobTable();});

    startB->setIcon(QIcon::fromTheme("media-playback-start"));
    startB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    pauseB->setIcon(QIcon::fromTheme("media-playback-pause"));
    pauseB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    pauseB->setCheckable(false);

    connect(startB, &QPushButton::clicked, this, &Scheduler::toggleScheduler);
    connect(pauseB, &QPushButton::clicked, this, &Scheduler::pause);

    connect(queueSaveAsB, &QPushButton::clicked, this, &Scheduler::saveAs);
    connect(queueSaveB, &QPushButton::clicked, this, &Scheduler::save);
    connect(queueLoadB, &QPushButton::clicked, this, [&]()
    {
        load(true);
    });
    connect(queueAppendB, &QPushButton::clicked, this, [&]()
    {
        load(false);
    });

    connect(schedulerTwilight, &QCheckBox::toggled, this, &Scheduler::checkTwilightWarning);

    // Connect simulation clock scale
    connect(KStarsData::Instance()->clock(), &SimClock::scaleChanged, this, &Scheduler::simClockScaleChanged);
    connect(KStarsData::Instance()->clock(), &SimClock::timeChanged, this, &Scheduler::simClockTimeChanged);

    // Connect to the state machine
    connect(moduleState().data(), &SchedulerModuleState::ekosStateChanged, this, &Scheduler::ekosStateChanged);
    connect(moduleState().data(), &SchedulerModuleState::indiStateChanged, this, &Scheduler::indiStateChanged);
    connect(moduleState().data(), &SchedulerModuleState::startupStateChanged, this, &Scheduler::startupStateChanged);
    connect(moduleState().data(), &SchedulerModuleState::shutdownStateChanged, this, &Scheduler::shutdownStateChanged);
    connect(moduleState().data(), &SchedulerModuleState::parkWaitStateChanged, this, &Scheduler::parkWaitStateChanged);
    connect(moduleState().data(), &SchedulerModuleState::profilesChanged, this, &Scheduler::updateProfiles);
    connect(moduleState().data(), &SchedulerModuleState::currentProfileChanged, this, [&]()
    {
        schedulerProfileCombo->setCurrentText(moduleState()->currentProfile());
    });
    // Connect to process engine
    connect(process().data(), &SchedulerProcess::newLog, this, &Scheduler::appendLogText);
    connect(process().data(), &SchedulerProcess::stopScheduler, this, &Scheduler::stop);
    connect(process().data(), &SchedulerProcess::stopCurrentJobAction, this, &Scheduler::stopCurrentJobAction);
    connect(process().data(), &SchedulerProcess::findNextJob, this, &Scheduler::findNextJob);
    connect(process().data(), &SchedulerProcess::getNextAction, this, &Scheduler::getNextAction);
    connect(process().data(), &SchedulerProcess::addJob, this, &Scheduler::addJob);
    connect(process().data(), &SchedulerProcess::syncGUIToGeneralSettings, this, &Scheduler::syncGUIToGeneralSettings);
    connect(process().data(), &SchedulerProcess::updateSchedulerURL, this, &Scheduler::updateSchedulerURL);
    connect(process().data(), &SchedulerProcess::newJobStage, this, &Scheduler::updateJobStageUI);
    // Connect geographical location - when it is available
    //connect(KStarsData::Instance()..., &LocationDialog::locationChanged..., this, &Scheduler::simClockTimeChanged);

    // Restore values for general settings.
    syncGUIToGeneralSettings();


    connect(errorHandlingButtonGroup, static_cast<void (QButtonGroup::*)(QAbstractButton *)>
            (&QButtonGroup::buttonClicked), [this](QAbstractButton * button)
    {
        Q_UNUSED(button)
        ErrorHandlingStrategy strategy = getErrorHandlingStrategy();
        Options::setErrorHandlingStrategy(strategy);
        errorHandlingStrategyDelay->setEnabled(strategy != ERROR_DONT_RESTART);
    });
    connect(errorHandlingStrategyDelay, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), [](int value)
    {
        Options::setErrorHandlingStrategyDelay(value);
    });

    // Retiring the Classic algorithm.
    if (Options::schedulerAlgorithm() != ALGORITHM_GREEDY)
    {
        appendLogText(i18n("Warning: The Classic scheduler algorithm has been retired. Switching you to the Greedy algorithm."));
        Options::setSchedulerAlgorithm(ALGORITHM_GREEDY);
    }

    // restore default values for scheduler algorithm
    setAlgorithm(Options::schedulerAlgorithm());

    connect(copySkyCenterB, &QPushButton::clicked, this, [this]()
    {
        SkyPoint center = SkyMap::Instance()->getCenterPoint();
        //center.deprecess(KStarsData::Instance()->updateNum());
        center.catalogueCoord(KStarsData::Instance()->updateNum()->julianDay());
        raBox->show(center.ra0());
        decBox->show(center.dec0());
    });

    connect(KConfigDialog::exists("settings"), &KConfigDialog::settingsChanged, this, &Scheduler::applyConfig);

    moduleState()->calculateDawnDusk();
    updateNightTime();

    process()->loadProfiles();

    watchJobChanges(true);

    loadGlobalSettings();
    connectSettings();
}

QString Scheduler::getCurrentJobName()
{
    return (activeJob() != nullptr ? activeJob()->getName() : "");
}

void Scheduler::watchJobChanges(bool enable)
{
    /* Don't double watch, this will cause multiple signals to be connected */
    if (enable == jobChangesAreWatched)
        return;

    /* These are the widgets we want to connect, per signal function, to listen for modifications */
    QLineEdit * const lineEdits[] =
    {
        nameEdit,
        groupEdit,
        raBox,
        decBox,
        fitsEdit,
        sequenceEdit,
        schedulerStartupScript,
        schedulerShutdownScript
    };

    QDateTimeEdit * const dateEdits[] =
    {
        startupTimeEdit,
        schedulerUntilValue
    };

    QComboBox * const comboBoxes[] =
    {
        schedulerProfileCombo,
    };

    QButtonGroup * const buttonGroups[] =
    {
        stepsButtonGroup,
        errorHandlingButtonGroup,
        startupButtonGroup,
        constraintButtonGroup,
        completionButtonGroup,
        startupProcedureButtonGroup,
        shutdownProcedureGroup
    };

    QAbstractButton * const buttons[] =
    {
        errorHandlingRescheduleErrorsCB
    };

    QSpinBox * const spinBoxes[] =
    {
        schedulerExecutionSequencesLimit,
        errorHandlingStrategyDelay
    };

    QDoubleSpinBox * const dspinBoxes[] =
    {
        schedulerMoonSeparationValue,
        schedulerAltitudeValue,
        positionAngleSpin,
    };

    if (enable)
    {
        /* Connect the relevant signal to setDirty. Note that we are not keeping the connection object: we will
         * only use that signal once, and there will be no leaks. If we were connecting multiple receiver functions
         * to the same signal, we would have to be selective when disconnecting. We also use a lambda to absorb the
         * excess arguments which cannot be passed to setDirty, and limit captured arguments to 'this'.
         * The main problem with this implementation compared to the macro method is that it is now possible to
         * stack signal connections. That is, multiple calls to WatchJobChanges will cause multiple signal-to-slot
         * instances to be registered. As a result, one click will produce N signals, with N*=2 for each call to
         * WatchJobChanges(true) missing its WatchJobChanges(false) counterpart.
         */
        for (auto * const control : lineEdits)
            connect(control, &QLineEdit::editingFinished, this, [this]()
        {
            setDirty();
        });
        for (auto * const control : dateEdits)
            connect(control, &QDateTimeEdit::editingFinished, this, [this]()
        {
            setDirty();
        });
        for (auto * const control : comboBoxes)
            connect(control, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [this]()
        {
            setDirty();
        });
        for (auto * const control : buttonGroups)
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
            connect(control, static_cast<void (QButtonGroup::*)(int, bool)>(&QButtonGroup::buttonToggled), this, [this](int, bool)
#else
            connect(control, static_cast<void (QButtonGroup::*)(int, bool)>(&QButtonGroup::idToggled), this, [this](int, bool)
#endif
        {
            setDirty();
        });
        for (auto * const control : buttons)
            connect(control, static_cast<void (QAbstractButton::*)(bool)>(&QAbstractButton::clicked), this, [this](bool)
        {
            setDirty();
        });
        for (auto * const control : spinBoxes)
            connect(control, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this]()
        {
            setDirty();
        });
        for (auto * const control : dspinBoxes)
            connect(control, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), this, [this](double)
        {
            setDirty();
        });
    }
    else
    {
        /* Disconnect the relevant signal from each widget. Actually, this method removes all signals from the widgets,
         * because we did not take care to keep the connection object when connecting. No problem in our case, we do not
         * expect other signals to be connected. Because we used a lambda, we cannot use the same function object to
         * disconnect selectively.
         */
        for (auto * const control : lineEdits)
            disconnect(control, &QLineEdit::editingFinished, this, nullptr);
        for (auto * const control : dateEdits)
            disconnect(control, &QDateTimeEdit::editingFinished, this, nullptr);
        for (auto * const control : comboBoxes)
            disconnect(control, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, nullptr);
        for (auto * const control : buttons)
            disconnect(control, static_cast<void (QAbstractButton::*)(bool)>(&QAbstractButton::clicked), this, nullptr);
        for (auto * const control : buttonGroups)
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
            disconnect(control, static_cast<void (QButtonGroup::*)(int, bool)>(&QButtonGroup::buttonToggled), this, nullptr);
#else
            disconnect(control, static_cast<void (QButtonGroup::*)(int, bool)>(&QButtonGroup::idToggled), this, nullptr);
#endif
        for (auto * const control : spinBoxes)
            disconnect(control, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, nullptr);
        for (auto * const control : dspinBoxes)
            disconnect(control, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), this, nullptr);
    }

    jobChangesAreWatched = enable;
}

void Scheduler::appendLogText(const QString &text)
{
    /* FIXME: user settings for log length */
    int const max_log_count = 2000;
    if (m_LogText.size() > max_log_count)
        m_LogText.removeLast();

    m_LogText.prepend(i18nc("log entry; %1 is the date, %2 is the text", "%1 %2",
                            SchedulerModuleState::getLocalTime().toString("yyyy-MM-ddThh:mm:ss"), text));

    qCInfo(KSTARS_EKOS_SCHEDULER) << text;

    emit newLog(text);
}

void Scheduler::clearLog()
{
    m_LogText.clear();
    emit newLog(QString());
}

void Scheduler::applyConfig()
{
    moduleState()->calculateDawnDusk();
    updateNightTime();
    repeatSequenceCB->setEnabled(Options::rememberJobProgress() == false);
    executionSequenceLimit->setEnabled(Options::rememberJobProgress() == false);

    if (SCHEDULER_RUNNING != moduleState()->schedulerState())
    {
        evaluateJobs(true);
    }
}

void Scheduler::selectObject()
{
    if (FindDialog::Instance()->execWithParent(Ekos::Manager::Instance()) == QDialog::Accepted)
    {
        SkyObject *object = FindDialog::Instance()->targetObject();
        addObject(object);
    }
}

void Scheduler::addObject(SkyObject *object)
{
    if (object != nullptr)
    {
        QString finalObjectName(object->name());

        if (object->name() == "star")
        {
            StarObject *s = dynamic_cast<StarObject *>(object);

            if (s->getHDIndex() != 0)
                finalObjectName = QString("HD %1").arg(s->getHDIndex());
        }

        nameEdit->setText(finalObjectName);
        raBox->show(object->ra0());
        decBox->show(object->dec0());

        addToQueueB->setEnabled(sequenceEdit->text().isEmpty() == false);
        //mosaicB->setEnabled(sequenceEdit->text().isEmpty() == false);

        setDirty();
    }
}

void Scheduler::selectFITS()
{
    auto url = QFileDialog::getOpenFileUrl(Ekos::Manager::Instance(), i18nc("@title:window", "Select FITS/XISF Image"), dirPath,
                                           "FITS (*.fits *.fit);;XISF (*.xisf)");
    if (url.isEmpty())
        return;

    processFITSSelection(url);
}

void Scheduler::processFITSSelection(const QUrl &url)
{
    if (url.isEmpty())
        return;

    fitsURL = url;
    dirPath = QUrl(fitsURL.url(QUrl::RemoveFilename));
    fitsEdit->setText(fitsURL.toLocalFile());
    if (nameEdit->text().isEmpty())
        nameEdit->setText(fitsURL.fileName());
    addToQueueB->setEnabled(sequenceEdit->text().isEmpty() == false);
    setDirty();

    const QString filename = fitsEdit->text();
    int status = 0;
    double ra = 0, dec = 0;
    dms raDMS, deDMS;
    char comment[128], error_status[512];
    fitsfile *fptr = nullptr;

    if (fits_open_diskfile(&fptr, filename.toLatin1(), READONLY, &status))
    {
        fits_report_error(stderr, status);
        fits_get_errstatus(status, error_status);
        qCCritical(KSTARS_EKOS_SCHEDULER) << QString::fromUtf8(error_status);
        return;
    }

    status = 0;
    if (fits_movabs_hdu(fptr, 1, IMAGE_HDU, &status))
    {
        fits_report_error(stderr, status);
        fits_get_errstatus(status, error_status);
        qCCritical(KSTARS_EKOS_SCHEDULER) << QString::fromUtf8(error_status);
        return;
    }

    status = 0;
    char objectra_str[32] = {0};
    if (fits_read_key(fptr, TSTRING, "OBJCTRA", objectra_str, comment, &status))
    {
        if (fits_read_key(fptr, TDOUBLE, "RA", &ra, comment, &status))
        {
            fits_report_error(stderr, status);
            fits_get_errstatus(status, error_status);
            appendLogText(i18n("FITS header: cannot find OBJCTRA (%1).", QString(error_status)));
            return;
        }

        raDMS.setD(ra);
    }
    else
    {
        raDMS = dms::fromString(objectra_str, false);
    }

    status = 0;
    char objectde_str[32] = {0};
    if (fits_read_key(fptr, TSTRING, "OBJCTDEC", objectde_str, comment, &status))
    {
        if (fits_read_key(fptr, TDOUBLE, "DEC", &dec, comment, &status))
        {
            fits_report_error(stderr, status);
            fits_get_errstatus(status, error_status);
            appendLogText(i18n("FITS header: cannot find OBJCTDEC (%1).", QString(error_status)));
            return;
        }

        deDMS.setD(dec);
    }
    else
    {
        deDMS = dms::fromString(objectde_str, true);
    }

    raBox->show(raDMS);
    decBox->show(deDMS);

    char object_str[256] = {0};
    if (fits_read_key(fptr, TSTRING, "OBJECT", object_str, comment, &status))
    {
        QFileInfo info(filename);
        nameEdit->setText(info.completeBaseName());
    }
    else
    {
        nameEdit->setText(object_str);
    }
}

void Scheduler::setSequence(const QString &sequenceFileURL)
{
    sequenceURL = QUrl::fromLocalFile(sequenceFileURL);

    if (sequenceFileURL.isEmpty())
        return;
    dirPath = QUrl(sequenceURL.url(QUrl::RemoveFilename));

    sequenceEdit->setText(sequenceURL.toLocalFile());

    // For object selection, all fields must be filled
    if ((raBox->isEmpty() == false && decBox->isEmpty() == false && nameEdit->text().isEmpty() == false)
            // For FITS selection, only the name and fits URL should be filled.
            || (nameEdit->text().isEmpty() == false && fitsURL.isEmpty() == false))
    {
        addToQueueB->setEnabled(true);
        //mosaicB->setEnabled(true);
    }

    setDirty();
}

void Scheduler::selectSequence()
{
    QString file = QFileDialog::getOpenFileName(Ekos::Manager::Instance(), i18nc("@title:window", "Select Sequence Queue"),
                   dirPath.toLocalFile(),
                   i18n("Ekos Sequence Queue (*.esq)"));

    setSequence(file);
}

void Scheduler::selectStartupScript()
{
    moduleState()->setStartupScriptURL(QFileDialog::getOpenFileUrl(Ekos::Manager::Instance(), i18nc("@title:window",
                                       "Select Startup Script"),
                                       dirPath,
                                       i18n("Script (*)")));
    if (moduleState()->startupScriptURL().isEmpty())
        return;

    dirPath = QUrl(moduleState()->startupScriptURL().url(QUrl::RemoveFilename));

    moduleState()->setDirty(true);
    schedulerStartupScript->setText(moduleState()->startupScriptURL().toLocalFile());
}

void Scheduler::selectShutdownScript()
{
    moduleState()->setShutdownScriptURL(QFileDialog::getOpenFileUrl(Ekos::Manager::Instance(), i18nc("@title:window",
                                        "Select Shutdown Script"),
                                        dirPath,
                                        i18n("Script (*)")));
    if (moduleState()->shutdownScriptURL().isEmpty())
        return;

    dirPath = QUrl(moduleState()->shutdownScriptURL().url(QUrl::RemoveFilename));

    moduleState()->setDirty(true);
    schedulerShutdownScript->setText(moduleState()->shutdownScriptURL().toLocalFile());
}

void Scheduler::addJob(SchedulerJob *job)
{
    if (0 <= jobUnderEdit)
    {
        // select the job currently being edited
        job = moduleState()->jobs().at(jobUnderEdit);
        // if existing, save it
        if (job != nullptr)
            saveJob(job);
        // in any case, reset editing
        resetJobEdit();
    }
    else
    {
        // remember the number of rows to select the first one appended
        int currentRow = queueTable->currentRow();

        //If no row is selected, the job will be appended at the end of the list, otherwise below the current selection
        if (currentRow < 0)
            currentRow = queueTable->rowCount();
        else
            currentRow++;

        /* If a job is being added, save fields into a new job */
        saveJob(job);
        addToQueueB->setEnabled(true);

        // select the first appended row (if any was added)
        if (moduleState()->jobs().count() > currentRow)
            queueTable->selectRow(currentRow);
    }

    emit jobsUpdated(getJSONJobs());
}

bool Scheduler::fillJobFromUI(SchedulerJob *job)
{
    if (nameEdit->text().isEmpty())
    {
        appendLogText(i18n("Warning: Target name is required."));
        return false;
    }

    if (sequenceEdit->text().isEmpty())
    {
        appendLogText(i18n("Warning: Sequence file is required."));
        return false;
    }

    // Coordinates are required unless it is a FITS file
    if ((raBox->isEmpty() || decBox->isEmpty()) && fitsURL.isEmpty())
    {
        appendLogText(i18n("Warning: Target coordinates are required."));
        return false;
    }

    bool raOk = false, decOk = false;
    dms /*const*/ ra(raBox->createDms(&raOk));
    dms /*const*/ dec(decBox->createDms(&decOk));

    if (raOk == false)
    {
        appendLogText(i18n("Warning: RA value %1 is invalid.", raBox->text()));
        return false;
    }

    if (decOk == false)
    {
        appendLogText(i18n("Warning: DEC value %1 is invalid.", decBox->text()));
        return false;
    }

    /* Configure or reconfigure the observation job */
    fitsURL = QUrl::fromLocalFile(fitsEdit->text());

    // Get several job values depending on the state of the UI.

    StartupCondition startCondition = START_AT;
    if (asapConditionR->isChecked())
        startCondition = START_ASAP;

    CompletionCondition stopCondition = FINISH_AT;
    if (schedulerCompleteSequences->isChecked())
        stopCondition = FINISH_SEQUENCE;
    else if (schedulerRepeatSequences->isChecked())
        stopCondition = FINISH_REPEAT;
    else if (schedulerUntilTerminated->isChecked())
        stopCondition = FINISH_LOOP;

    double altConstraint = SchedulerJob::UNDEFINED_ALTITUDE;
    if (schedulerAltitude->isChecked())
        altConstraint = schedulerAltitudeValue->value();

    double moonConstraint = -1;
    if (schedulerMoonSeparation->isChecked())
        moonConstraint = schedulerMoonSeparationValue->value();

    // The reason for this kitchen-sink function is to separate the UI from the
    // job setup, to allow for testing.
    SchedulerUtils::setupJob(*job, nameEdit->text(), groupEdit->text(), ra, dec,
                             KStarsData::Instance()->ut().djd(),
                             positionAngleSpin->value(), sequenceURL, fitsURL,

                             startCondition, startupTimeEdit->dateTime(),
                             stopCondition, schedulerUntilValue->dateTime(), schedulerExecutionSequencesLimit->value(),

                             altConstraint,
                             moonConstraint,
                             schedulerWeather->isChecked(),
                             schedulerTwilight->isChecked(),
                             schedulerHorizon->isChecked(),

                             schedulerTrackStep->isChecked(),
                             schedulerFocusStep->isChecked(),
                             schedulerAlignStep->isChecked(),
                             schedulerGuideStep->isChecked());

    // success
    updateJobTable(job);
    return true;
}

void Scheduler::saveJob(SchedulerJob *job)
{
    watchJobChanges(false);

    /* Create or Update a scheduler job */
    int currentRow = queueTable->currentRow();

    /* If no row is selected for insertion, append at end of list. Otherwise append below current selection */
    if (currentRow < 0)
        currentRow = queueTable->rowCount();
    else
        currentRow++;

    /* Add job to queue only if it is new, else reuse current row.
     * Make sure job is added at the right index, now that queueTable may have a line selected without being edited.
     */
    if (0 <= jobUnderEdit)
    {
        /* FIXME: jobUnderEdit is a parallel variable that may cause issues if it desyncs from queueTable->currentRow(). */
        if (jobUnderEdit != currentRow - 1)
            qCWarning(KSTARS_EKOS_SCHEDULER) << "BUG: the observation job under edit does not match the selected row in the job table.";

        /* Use the job in the row currently edited */
        job = moduleState()->jobs().at(jobUnderEdit);
        // try to fill the job from the UI and exit if it fails
        if (fillJobFromUI(job) == false)
            return;
    }
    else
    {
        if (job == nullptr)
        {
            /* Instantiate a new job, insert it in the job list and add a row in the table for it just after the row currently selected. */
            job = new SchedulerJob();
            // try to fill the job from the UI and exit if it fails
            if (fillJobFromUI(job) == false)
            {
                delete(job);
                return;
            }
        }
        /* Insert the job in the job list and add a row in the table for it just after the row currently selected. */
        moduleState()->mutlableJobs().insert(currentRow, job);
        insertJobTableRow(currentRow);
    }

    /* Verifications */
    // Warn user if a duplicated job is in the list - same target, same sequence
    // FIXME: Those duplicated jobs are not necessarily processed in the order they appear in the list!
    int numWarnings = 0;
    foreach (SchedulerJob *a_job, moduleState()->jobs())
    {
        if (a_job == job)
        {
            break;
        }
        else if (a_job->getName() == job->getName())
        {
            int const a_job_row = moduleState()->jobs().indexOf(a_job);

            /* FIXME: Warning about duplicate jobs only checks the target name, doing it properly would require checking storage for each sequence job of each scheduler job. */
            appendLogText(i18n("Warning: job '%1' at row %2 has a duplicate target at row %3, "
                               "the scheduler may consider the same storage for captures.",
                               job->getName(), currentRow, a_job_row));

            /* Warn the user in case the two jobs are really identical */
            if (a_job->getSequenceFile() == job->getSequenceFile())
            {
                if (a_job->getRepeatsRequired() == job->getRepeatsRequired() && Options::rememberJobProgress())
                    appendLogText(i18n("Warning: jobs '%1' at row %2 and %3 probably require a different repeat count "
                                       "as currently they will complete simultaneously after %4 batches (or disable option 'Remember job progress')",
                                       job->getName(), currentRow, a_job_row, job->getRepeatsRequired()));
            }

            // Don't need to warn over and over.
            if (++numWarnings >= 1)
            {
                appendLogText(i18n("Skipped checking for duplicates."));
                break;
            }
        }
    }

    updateJobTable(job);

    /* We just added or saved a job, so we have a job in the list - enable relevant buttons */
    queueSaveAsB->setEnabled(true);
    queueSaveB->setEnabled(true);
    startB->setEnabled(true);
    evaluateOnlyB->setEnabled(true);
    setJobManipulation(true, true);

    qCDebug(KSTARS_EKOS_SCHEDULER) << QString("Job '%1' at row #%2 was saved.").arg(job->getName()).arg(currentRow + 1);

    watchJobChanges(true);

    if (SCHEDULER_LOADING != moduleState()->schedulerState())
    {
        evaluateJobs(true);
    }
}

void Scheduler::syncGUIToJob(SchedulerJob *job)
{
    nameEdit->setText(job->getName());
    groupEdit->setText(job->getGroup());

    raBox->show(job->getTargetCoords().ra0());
    decBox->show(job->getTargetCoords().dec0());

    // fitsURL/sequenceURL are not part of UI, but the UI serves as model, so keep them here for now
    fitsURL = job->getFITSFile().isEmpty() ? QUrl() : job->getFITSFile();
    sequenceURL = job->getSequenceFile();
    fitsEdit->setText(fitsURL.toLocalFile());
    sequenceEdit->setText(sequenceURL.toLocalFile());

    positionAngleSpin->setValue(job->getPositionAngle());

    schedulerTrackStep->setChecked(job->getStepPipeline() & SchedulerJob::USE_TRACK);
    schedulerFocusStep->setChecked(job->getStepPipeline() & SchedulerJob::USE_FOCUS);
    schedulerAlignStep->setChecked(job->getStepPipeline() & SchedulerJob::USE_ALIGN);
    schedulerGuideStep->setChecked(job->getStepPipeline() & SchedulerJob::USE_GUIDE);

    switch (job->getFileStartupCondition())
    {
        case START_ASAP:
            asapConditionR->setChecked(true);
            break;

        case START_AT:
            startupTimeConditionR->setChecked(true);
            startupTimeEdit->setDateTime(job->getStartupTime());
            break;
    }

    if (job->getMinAltitude())
    {
        schedulerAltitude->setChecked(true);
        schedulerAltitudeValue->setValue(job->getMinAltitude());
    }
    else
    {
        schedulerAltitude->setChecked(false);
        schedulerAltitudeValue->setValue(DEFAULT_MIN_ALTITUDE);
    }

    if (job->getMinMoonSeparation() >= 0)
    {
        schedulerMoonSeparation->setChecked(true);
        schedulerMoonSeparationValue->setValue(job->getMinMoonSeparation());
    }
    else
    {
        schedulerMoonSeparation->setChecked(false);
        schedulerMoonSeparationValue->setValue(DEFAULT_MIN_MOON_SEPARATION);
    }

    schedulerWeather->setChecked(job->getEnforceWeather());

    schedulerTwilight->blockSignals(true);
    schedulerTwilight->setChecked(job->getEnforceTwilight());
    schedulerTwilight->blockSignals(false);

    schedulerHorizon->blockSignals(true);
    schedulerHorizon->setChecked(job->getEnforceArtificialHorizon());
    schedulerHorizon->blockSignals(false);

    switch (job->getCompletionCondition())
    {
        case FINISH_SEQUENCE:
            schedulerCompleteSequences->setChecked(true);
            break;

        case FINISH_REPEAT:
            schedulerRepeatSequences->setChecked(true);
            schedulerExecutionSequencesLimit->setValue(job->getRepeatsRequired());
            break;

        case FINISH_LOOP:
            schedulerUntilTerminated->setChecked(true);
            break;

        case FINISH_AT:
            schedulerUntil->setChecked(true);
            schedulerUntilValue->setDateTime(job->getCompletionTime());
            break;
    }

    updateNightTime(job);

    setJobManipulation(true, true);
}

void Scheduler::syncGUIToGeneralSettings()
{
    schedulerParkDome->setChecked(Options::schedulerParkDome());
    schedulerParkMount->setChecked(Options::schedulerParkMount());
    schedulerCloseDustCover->setChecked(Options::schedulerCloseDustCover());
    schedulerWarmCCD->setChecked(Options::schedulerWarmCCD());
    schedulerUnparkDome->setChecked(Options::schedulerUnparkDome());
    schedulerUnparkMount->setChecked(Options::schedulerUnparkMount());
    schedulerOpenDustCover->setChecked(Options::schedulerOpenDustCover());
    setErrorHandlingStrategy(static_cast<ErrorHandlingStrategy>(Options::errorHandlingStrategy()));
    errorHandlingStrategyDelay->setValue(Options::errorHandlingStrategyDelay());
    errorHandlingRescheduleErrorsCB->setChecked(Options::rescheduleErrors());
    schedulerStartupScript->setText(moduleState()->startupScriptURL().toString(QUrl::PreferLocalFile));
    schedulerShutdownScript->setText(moduleState()->shutdownScriptURL().toString(QUrl::PreferLocalFile));

    if (process()->captureInterface() != nullptr)
    {
        QVariant hasCoolerControl = process()->captureInterface()->property("coolerControl");
        if (hasCoolerControl.isValid())
        {
            schedulerWarmCCD->setEnabled(hasCoolerControl.toBool());
            moduleState()->setCaptureReady(true);
        }
    }
}

void Scheduler::updateNightTime(SchedulerJob const *job)
{
    if (job == nullptr)
    {
        int const currentRow = queueTable->currentRow();
        if (0 < currentRow)
            job = moduleState()->jobs().at(currentRow);
    }

    QDateTime const dawn = job ? job->getDawnAstronomicalTwilight() : moduleState()->Dawn();
    QDateTime const dusk = job ? job->getDuskAstronomicalTwilight() : moduleState()->Dusk();

    QChar const warning(dawn == dusk ? 0x26A0 : '-');
    nightTime->setText(i18n("%1 %2 %3", dusk.toString("hh:mm"), warning, dawn.toString("hh:mm")));
}

void Scheduler::loadJob(QModelIndex i)
{
    if (jobUnderEdit == i.row())
        return;

    SchedulerJob * const job = moduleState()->jobs().at(i.row());

    if (job == nullptr)
        return;

    watchJobChanges(false);

    //job->setState(SCHEDJOB_IDLE);
    //job->setStage(SCHEDSTAGE_IDLE);
    syncGUIToJob(job);

    /* Turn the add button into an apply button */
    setJobAddApply(false);

    /* Disable scheduler start/evaluate buttons */
    startB->setEnabled(false);
    evaluateOnlyB->setEnabled(false);

    /* Don't let the end-user remove a job being edited */
    setJobManipulation(false, false);

    jobUnderEdit = i.row();
    qCDebug(KSTARS_EKOS_SCHEDULER) << QString("Job '%1' at row #%2 is currently edited.").arg(job->getName()).arg(
                                       jobUnderEdit + 1);

    watchJobChanges(true);
}

void Scheduler::updateSchedulerURL(const QString &fileURL)
{
    schedulerURL = QUrl::fromLocalFile(fileURL);
    // update save button tool tip
    queueSaveB->setToolTip("Save schedule to " + schedulerURL.fileName());
}

void Scheduler::queueTableSelectionChanged(QModelIndex current, QModelIndex previous)
{
    Q_UNUSED(previous)

    if (current.row() < 0 || (current.row() + 1) > moduleState()->jobs().size())
        return;

    SchedulerJob * const job = moduleState()->jobs().at(current.row());

    if (job != nullptr)
    {
        if (jobUnderEdit < 0)
            syncGUIToJob(job);
        else if (jobUnderEdit != current.row())
        {
            // avoid changing the UI values for the currently edited job
            appendLogText(i18n("Stop editing of job #%1, resetting to original value.", jobUnderEdit+1));
            resetJobEdit();
            syncGUIToJob(job);
        }
    }
    else nightTime->setText("-");
}

void Scheduler::clickQueueTable(QModelIndex index)
{
    setJobManipulation(index.isValid(), index.isValid());
}

void Scheduler::setJobAddApply(bool add_mode)
{
    if (add_mode)
    {
        addToQueueB->setIcon(QIcon::fromTheme("list-add"));
        addToQueueB->setToolTip(i18n("Use edition fields to create a new job in the observation list."));
        //addToQueueB->setStyleSheet(QString());
        addToQueueB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    }
    else
    {
        addToQueueB->setIcon(QIcon::fromTheme("dialog-ok-apply"));
        addToQueueB->setToolTip(i18n("Apply job changes."));
        //addToQueueB->setStyleSheet("background-color:orange;}");
        addToQueueB->setEnabled(true);
    }
}

void Scheduler::setJobManipulation(bool can_reorder, bool can_delete)
{
    if (can_reorder)
    {
        int const currentRow = queueTable->currentRow();
        queueUpB->setEnabled(0 < currentRow);
        queueDownB->setEnabled(currentRow < queueTable->rowCount() - 1);
    }
    else
    {
        queueUpB->setEnabled(false);
        queueDownB->setEnabled(false);
    }
    sortJobsB->setEnabled(can_reorder);
    removeFromQueueB->setEnabled(can_delete);
}

bool Scheduler::reorderJobs(QList<SchedulerJob*> reordered_sublist)
{
    /* Add jobs not reordered at the end of the list, in initial order */
    foreach (SchedulerJob* job, moduleState()->jobs())
        if (!reordered_sublist.contains(job))
            reordered_sublist.append(job);

    if (moduleState()->jobs() != reordered_sublist)
    {
        /* Remember job currently selected */
        int const selectedRow = queueTable->currentRow();
        SchedulerJob * const selectedJob = 0 <= selectedRow ? moduleState()->jobs().at(selectedRow) : nullptr;

        /* Reassign list */
        moduleState()->setJobs(reordered_sublist);

        /* Refresh the table */
        for (SchedulerJob *job: moduleState()->jobs())
            updateJobTable(job);

        /* Reselect previously selected job */
        if (nullptr != selectedJob)
            queueTable->selectRow(moduleState()->jobs().indexOf(selectedJob));

        return true;
    }
    else return false;
}

void Scheduler::moveJobUp()
{
    int const rowCount = queueTable->rowCount();
    int const currentRow = queueTable->currentRow();
    int const destinationRow = currentRow - 1;

    /* No move if no job selected, if table has one line or less or if destination is out of table */
    if (currentRow < 0 || rowCount <= 1 || destinationRow < 0)
        return;

    /* Swap jobs in the list */
#if QT_VERSION >= QT_VERSION_CHECK(5,13,0)
    moduleState()->mutlableJobs().swapItemsAt(currentRow, destinationRow);
#else
    moduleState()->jobs().swap(currentRow, destinationRow);
#endif

    //Update the two table rows
    updateJobTable(moduleState()->jobs().at(currentRow));
    updateJobTable(moduleState()->jobs().at(destinationRow));

    /* Move selection to destination row */
    queueTable->selectRow(destinationRow);
    setJobManipulation(true, true);

    /* Make list modified and evaluate jobs */
    moduleState()->setDirty(true);
    evaluateJobs(true);
}

void Scheduler::moveJobDown()
{
    int const rowCount = queueTable->rowCount();
    int const currentRow = queueTable->currentRow();
    int const destinationRow = currentRow + 1;

    /* No move if no job selected, if table has one line or less or if destination is out of table */
    if (currentRow < 0 || rowCount <= 1 || destinationRow == rowCount)
        return;

    /* Swap jobs in the list */
#if QT_VERSION >= QT_VERSION_CHECK(5,13,0)
    moduleState()->mutlableJobs().swapItemsAt(currentRow, destinationRow);
#else
    moduleState()->mutlableJobs().swap(currentRow, destinationRow);
#endif

    //Update the two table rows
    updateJobTable(moduleState()->jobs().at(currentRow));
    updateJobTable(moduleState()->jobs().at(destinationRow));

    /* Move selection to destination row */
    queueTable->selectRow(destinationRow);
    setJobManipulation(true, true);

    /* Make list modified and evaluate jobs */
    moduleState()->setDirty(true);
    evaluateJobs(true);
}

void Scheduler::updateJobTable(SchedulerJob *job)
{
    // handle full table update
    if (job == nullptr)
    {
        for (auto onejob: moduleState()->jobs())
            updateJobTable(onejob);

        return;
    }

    const int row = moduleState()->jobs().indexOf(job);
    // Ignore unknown jobs
    if (row < 0)
        return;
    // ensure that the row in the table exists
    if (row >= queueTable->rowCount())
        insertJobTableRow(row - 1, false);

    QTableWidgetItem *nameCell = queueTable->item(row, static_cast<int>(SCHEDCOL_NAME));
    QTableWidgetItem *statusCell = queueTable->item(row, static_cast<int>(SCHEDCOL_STATUS));
    QTableWidgetItem *altitudeCell = queueTable->item(row, static_cast<int>(SCHEDCOL_ALTITUDE));
    QTableWidgetItem *startupCell = queueTable->item(row, static_cast<int>(SCHEDCOL_STARTTIME));
    QTableWidgetItem *completionCell = queueTable->item(row, static_cast<int>(SCHEDCOL_ENDTIME));
    QTableWidgetItem *captureCountCell = queueTable->item(row, static_cast<int>(SCHEDCOL_CAPTURES));

    // Only in testing.
    if (!nameCell) return;

    if (nullptr != nameCell)
    {
        nameCell->setText(job->getName());
        updateCellStyle(job, nameCell);
        if (nullptr != nameCell->tableWidget())
            nameCell->tableWidget()->resizeColumnToContents(nameCell->column());
    }

    if (nullptr != statusCell)
    {
        static QMap<SchedulerJobStatus, QString> stateStrings;
        static QString stateStringUnknown;
        if (stateStrings.isEmpty())
        {
            stateStrings[SCHEDJOB_IDLE] = i18n("Idle");
            stateStrings[SCHEDJOB_EVALUATION] = i18n("Evaluating");
            stateStrings[SCHEDJOB_SCHEDULED] = i18n("Scheduled");
            stateStrings[SCHEDJOB_BUSY] = i18n("Running");
            stateStrings[SCHEDJOB_INVALID] = i18n("Invalid");
            stateStrings[SCHEDJOB_COMPLETE] = i18n("Complete");
            stateStrings[SCHEDJOB_ABORTED] = i18n("Aborted");
            stateStrings[SCHEDJOB_ERROR] =  i18n("Error");
            stateStringUnknown = i18n("Unknown");
        }
        statusCell->setText(stateStrings.value(job->getState(), stateStringUnknown));
        updateCellStyle(job, statusCell);

        if (nullptr != statusCell->tableWidget())
            statusCell->tableWidget()->resizeColumnToContents(statusCell->column());
    }

    if (nullptr != startupCell)
    {
        auto time = (job->getState() == SCHEDJOB_BUSY) ? job->getStateTime() : job->getStartupTime();
        /* Display startup time if it is valid */
        if (time.isValid())
        {
            startupCell->setText(QString("%1%2%L3° %4")
                                 .arg(job->getAltitudeAtStartup() < job->getMinAltitude() ? QString(QChar(0x26A0)) : "")
                                 .arg(QChar(job->isSettingAtStartup() ? 0x2193 : 0x2191))
                                 .arg(job->getAltitudeAtStartup(), 0, 'f', 1)
                                 .arg(time.toString(startupTimeEdit->displayFormat())));

            switch (job->getFileStartupCondition())
            {
                /* If the original condition is START_AT/START_CULMINATION, startup time is fixed */
                case START_AT:
                    startupCell->setIcon(QIcon::fromTheme("chronometer"));
                    break;

                /* If the original condition is START_ASAP, startup time is informational */
                case START_ASAP:
                    startupCell->setIcon(QIcon());
                    break;

                default:
                    break;
            }
        }
        /* Else do not display any startup time */
        else
        {
            startupCell->setText("-");
            startupCell->setIcon(QIcon());
        }

        updateCellStyle(job, startupCell);

        if (nullptr != startupCell->tableWidget())
            startupCell->tableWidget()->resizeColumnToContents(startupCell->column());
    }

    if (nullptr != altitudeCell)
    {
        // FIXME: Cache altitude calculations
        bool is_setting = false;
        double const alt = SchedulerUtils::findAltitude(job->getTargetCoords(), QDateTime(), &is_setting);

        altitudeCell->setText(QString("%1%L2°")
                              .arg(QChar(is_setting ? 0x2193 : 0x2191))
                              .arg(alt, 0, 'f', 1));
        updateCellStyle(job, altitudeCell);

        if (nullptr != altitudeCell->tableWidget())
            altitudeCell->tableWidget()->resizeColumnToContents(altitudeCell->column());
    }

    if (nullptr != completionCell)
    {
        if (job->getGreedyCompletionTime().isValid())
        {
            completionCell->setText(QString("%1")
                                    .arg(job->getGreedyCompletionTime().toString("hh:mm")));
        }
        else
            /* Display completion time if it is valid and job is not looping */
            if (FINISH_LOOP != job->getCompletionCondition() && job->getCompletionTime().isValid())
            {
                completionCell->setText(QString("%1%2%L3° %4")
                                        .arg(job->getAltitudeAtCompletion() < job->getMinAltitude() ? QString(QChar(0x26A0)) : "")
                                        .arg(QChar(job->isSettingAtCompletion() ? 0x2193 : 0x2191))
                                        .arg(job->getAltitudeAtCompletion(), 0, 'f', 1)
                                        .arg(job->getCompletionTime().toString(startupTimeEdit->displayFormat())));

                switch (job->getCompletionCondition())
                {
                    case FINISH_AT:
                        completionCell->setIcon(QIcon::fromTheme("chronometer"));
                        break;

                    case FINISH_SEQUENCE:
                    case FINISH_REPEAT:
                    default:
                        completionCell->setIcon(QIcon());
                        break;
                }
            }
        /* Else do not display any completion time */
            else
            {
                completionCell->setText("-");
                completionCell->setIcon(QIcon());
            }

        updateCellStyle(job, completionCell);
        if (nullptr != completionCell->tableWidget())
            completionCell->tableWidget()->resizeColumnToContents(completionCell->column());
    }

    if (nullptr != captureCountCell)
    {
        switch (job->getCompletionCondition())
        {
            case FINISH_AT:
            // FIXME: Attempt to calculate the number of frames until end - requires detailed imaging time

            case FINISH_LOOP:
                // If looping, display the count of completed frames
                captureCountCell->setText(QString("%L1/-").arg(job->getCompletedCount()));
                break;

            case FINISH_SEQUENCE:
            case FINISH_REPEAT:
            default:
                // If repeating, display the count of completed frames to the count of requested frames
                captureCountCell->setText(QString("%L1/%L2").arg(job->getCompletedCount()).arg(job->getSequenceCount()));
                break;
        }

        updateCellStyle(job, captureCountCell);
        if (nullptr != captureCountCell->tableWidget())
            captureCountCell->tableWidget()->resizeColumnToContents(captureCountCell->column());
    }

    emit jobsUpdated(getJSONJobs());
}

void Scheduler::insertJobTableRow(int row, bool above)
{
    const int pos = above ? row : row + 1;

    // ensure that there are no gaps
    if (row > queueTable->rowCount())
        insertJobTableRow(row - 1, above);

    queueTable->insertRow(pos);

    QTableWidgetItem *nameCell = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_NAME), nameCell);
    nameCell->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    nameCell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QTableWidgetItem *statusCell = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_STATUS), statusCell);
    statusCell->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    statusCell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QTableWidgetItem *captureCount = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_CAPTURES), captureCount);
    captureCount->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    captureCount->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QTableWidgetItem *startupCell = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_STARTTIME), startupCell);
    startupCell->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    startupCell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QTableWidgetItem *altitudeCell = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_ALTITUDE), altitudeCell);
    altitudeCell->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    altitudeCell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    QTableWidgetItem *completionCell = new QTableWidgetItem();
    queueTable->setItem(row, static_cast<int>(SCHEDCOL_ENDTIME), completionCell);
    completionCell->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    completionCell->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
}

void Scheduler::updateCellStyle(SchedulerJob *job, QTableWidgetItem *cell)
{
    QFont font(cell->font());
    font.setBold(job->getState() == SCHEDJOB_BUSY);
    font.setItalic(job->getState() == SCHEDJOB_BUSY);
    cell->setFont(font);
}

void Scheduler::resetJobEdit()
{
    if (jobUnderEdit < 0)
        return;

    SchedulerJob * const job = moduleState()->jobs().at(jobUnderEdit);
    Q_ASSERT_X(job != nullptr, __FUNCTION__, "Edited job must be valid");

    qCDebug(KSTARS_EKOS_SCHEDULER) << QString("Job '%1' at row #%2 is not longer edited.").arg(job->getName()).arg(
                                       jobUnderEdit + 1);

    jobUnderEdit = -1;

    watchJobChanges(false);

    /* Revert apply button to add */
    setJobAddApply(true);

    /* Refresh state of job manipulation buttons */
    setJobManipulation(true, true);

    /* Restore scheduler operation buttons */
    evaluateOnlyB->setEnabled(true);
    startB->setEnabled(true);

    Q_ASSERT_X(jobUnderEdit == -1, __FUNCTION__, "No more edited/selected job after exiting edit mode");
}

void Scheduler::removeJob()
{
    int currentRow = queueTable->currentRow();

    /* Don't remove a row that is not selected */
    if (currentRow < 0)
        return;

    /* Grab the job currently selected */
    SchedulerJob * const job = moduleState()->jobs().at(currentRow);

    // Can't delete the currently running job
    if (job == activeJob())
    {
        appendLogText(i18n("Cannot delete currently running job '%1'.", job->getName()));
        return;
    }
    else if (job == nullptr || (activeJob() == nullptr && moduleState()->schedulerState() != SCHEDULER_IDLE))
    {
        // Don't allow delete--worried that we're about to schedule job that's being deleted.
        appendLogText(i18n("Cannot delete job. Scheduler state: %1",
                           getSchedulerStatusString(moduleState()->schedulerState(), true)));
        return;

    }

    qCDebug(KSTARS_EKOS_SCHEDULER) << QString("Job '%1' at row #%2 is being deleted.").arg(job->getName()).arg(currentRow + 1);

    /* Remove the job from the table */
    queueTable->removeRow(currentRow);

    /* If there are no job rows left, update UI buttons */
    if (queueTable->rowCount() == 0)
    {
        setJobManipulation(false, false);
        evaluateOnlyB->setEnabled(false);
        queueSaveAsB->setEnabled(false);
        queueSaveB->setEnabled(false);
        startB->setEnabled(false);
        pauseB->setEnabled(false);
    }

    /* Else update the selection */
    else
    {
        if (currentRow > queueTable->rowCount())
            currentRow = queueTable->rowCount() - 1;

        loadJob(queueTable->currentIndex());
        queueTable->selectRow(currentRow);
    }

    /* If needed, reset edit mode to clean up UI */
    if (jobUnderEdit >= 0)
        resetJobEdit();

    /* And remove the job object */
    moduleState()->mutlableJobs().removeOne(job);
    delete (job);

    moduleState()->setDirty(true);
    evaluateJobs(true);
    emit jobsUpdated(getJSONJobs());
    updateJobTable();
}

void Scheduler::removeOneJob(int index)
{
    queueTable->selectRow(index);
    removeJob();
}
void Scheduler::toggleScheduler()
{
    if (moduleState()->schedulerState() == SCHEDULER_RUNNING)
    {
        moduleState()->disablePreemptiveShutdown();
        stop();
    }
    else
        start();
}

void Scheduler::stop()
{
    if (moduleState()->schedulerState() != SCHEDULER_RUNNING)
        return;

    qCInfo(KSTARS_EKOS_SCHEDULER) << "Scheduler is stopping...";

    // Stop running job and abort all others
    // in case of soft shutdown we skip this
    if (!moduleState()->preemptiveShutdown())
    {
        bool wasAborted = false;
        for (auto &oneJob : moduleState()->jobs())
        {
            if (oneJob == activeJob())
                stopCurrentJobAction();

            if (oneJob->getState() <= SCHEDJOB_BUSY)
            {
                appendLogText(i18n("Job '%1' has not been processed upon scheduler stop, marking aborted.", oneJob->getName()));
                oneJob->setState(SCHEDJOB_ABORTED);
                updateJobTable(oneJob);
                wasAborted = true;
            }
        }

        if (wasAborted)
            KSNotification::event(QLatin1String("SchedulerAborted"), i18n("Scheduler aborted."), KSNotification::Scheduler,
                                  KSNotification::Alert);
    }

    TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_NOTHING).toLatin1().data());
    moduleState()->setupNextIteration(RUN_NOTHING);
    moduleState()->cancelGuidingTimer();

    moduleState()->setSchedulerState(SCHEDULER_IDLE);
    emit newStatus(moduleState()->schedulerState());
    moduleState()->setEkosState(EKOS_IDLE);
    moduleState()->setIndiState(INDI_IDLE);

    moduleState()->setParkWaitState(PARKWAIT_IDLE);

    // Only reset startup state to idle if the startup procedure was interrupted before it had the chance to complete.
    // Or if we're doing a soft shutdown
    if (moduleState()->startupState() != STARTUP_COMPLETE || moduleState()->preemptiveShutdown())
    {
        if (moduleState()->startupState() == STARTUP_SCRIPT)
        {
            process()->scriptProcess().disconnect();
            process()->scriptProcess().terminate();
        }

        moduleState()->setStartupState(STARTUP_IDLE);
    }
    // Reset startup state to unparking phase (dome -> mount -> cap)
    // We do not want to run the startup script again but unparking should be checked
    // whenever the scheduler is running again.
    else if (moduleState()->startupState() == STARTUP_COMPLETE)
    {
        if (schedulerUnparkDome->isChecked())
            moduleState()->setStartupState(STARTUP_UNPARK_DOME);
        else if (schedulerUnparkMount->isChecked())
            moduleState()->setStartupState(STARTUP_UNPARK_MOUNT);
        else if (schedulerOpenDustCover->isChecked())
            moduleState()->setStartupState(STARTUP_UNPARK_CAP);
    }

    moduleState()->setShutdownState(SHUTDOWN_IDLE);

    setActiveJob(nullptr);
    moduleState()->resetFailureCounters();
    moduleState()->setAutofocusCompleted(false);

    startupB->setEnabled(true);
    shutdownB->setEnabled(true);

    // If soft shutdown, we return for now
    if (moduleState()->preemptiveShutdown())
    {
        sleepLabel->setToolTip(i18n("Scheduler is in shutdown until next job is ready"));
        sleepLabel->show();

        QDateTime const now = SchedulerModuleState::getLocalTime();
        int const nextObservationTime = now.secsTo(moduleState()->preemptiveShutdownWakeupTime());
        moduleState()->setupNextIteration(RUN_WAKEUP,
                                          std::lround(((nextObservationTime + 1) * 1000)
                                                  / KStarsData::Instance()->clock()->scale()));
        return;

    }

    // Clear target name in capture interface upon stopping
    if (process()->captureInterface().isNull() == false)
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "captureInterface:setProperty", "targetName=\"\"");
        process()->captureInterface()->setProperty("targetName", QString());
    }

    if (process()->scriptProcess().state() == QProcess::Running)
        process()->scriptProcess().terminate();

    sleepLabel->hide();
    pi->stopAnimation();

    startB->setIcon(QIcon::fromTheme("media-playback-start"));
    startB->setToolTip(i18n("Start Scheduler"));
    pauseB->setEnabled(false);
    //startB->setText("Start Scheduler");

    queueLoadB->setEnabled(true);
    queueAppendB->setEnabled(true);
    addToQueueB->setEnabled(true);
    setJobManipulation(false, false);
    //mosaicB->setEnabled(true);
    evaluateOnlyB->setEnabled(true);
}

void Scheduler::execute()
{
    switch (moduleState()->schedulerState())
    {
        case SCHEDULER_IDLE:
            /* FIXME: Manage the non-validity of the startup script earlier, and make it a warning only when the scheduler starts */
            if (!moduleState()->startupScriptURL().isEmpty() && ! moduleState()->startupScriptURL().isValid())
            {
                appendLogText(i18n("Warning: startup script URL %1 is not valid.",
                                   moduleState()->startupScriptURL().toString(QUrl::PreferLocalFile)));
                return;
            }

            /* FIXME: Manage the non-validity of the shutdown script earlier, and make it a warning only when the scheduler starts */
            if (!moduleState()->shutdownScriptURL().isEmpty() && !moduleState()->shutdownScriptURL().isValid())
            {
                appendLogText(i18n("Warning: shutdown script URL %1 is not valid.",
                                   moduleState()->shutdownScriptURL().toString(QUrl::PreferLocalFile)));
                return;
            }

            qCInfo(KSTARS_EKOS_SCHEDULER) << "Scheduler is starting...";

            /* Update UI to reflect startup */
            pi->startAnimation();
            sleepLabel->hide();
            startB->setIcon(QIcon::fromTheme("media-playback-stop"));
            startB->setToolTip(i18n("Stop Scheduler"));
            pauseB->setEnabled(true);
            pauseB->setChecked(false);

            /* Disable edit-related buttons */
            queueLoadB->setEnabled(false);
            setJobManipulation(true, false);
            //mosaicB->setEnabled(false);
            evaluateOnlyB->setEnabled(false);
            startupB->setEnabled(false);
            shutdownB->setEnabled(false);

            moduleState()->setSchedulerState(SCHEDULER_RUNNING);
            emit newStatus(moduleState()->schedulerState());
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
            moduleState()->setupNextIteration(RUN_SCHEDULER);

            appendLogText(i18n("Scheduler started."));
            qCDebug(KSTARS_EKOS_SCHEDULER) << "Scheduler started.";
            break;

        case SCHEDULER_PAUSED:
            /* Update UI to reflect resume */
            startB->setIcon(QIcon::fromTheme("media-playback-stop"));
            startB->setToolTip(i18n("Stop Scheduler"));
            pauseB->setEnabled(true);
            pauseB->setCheckable(false);
            pauseB->setChecked(false);

            /* Edit-related buttons are still disabled */

            /* The end-user cannot update the schedule, don't re-evaluate jobs. Timer schedulerTimer is already running. */
            moduleState()->setSchedulerState(SCHEDULER_RUNNING);
            emit newStatus(moduleState()->schedulerState());
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
            moduleState()->setupNextIteration(RUN_SCHEDULER);

            appendLogText(i18n("Scheduler resuming."));
            qCDebug(KSTARS_EKOS_SCHEDULER) << "Scheduler resuming.";
            break;

        default:
            break;
    }
}

void Scheduler::pause()
{
    moduleState()->setSchedulerState(SCHEDULER_PAUSED);
    emit newStatus(moduleState()->schedulerState());
    appendLogText(i18n("Scheduler pause planned..."));
    pauseB->setEnabled(false);

    startB->setIcon(QIcon::fromTheme("media-playback-start"));
    startB->setToolTip(i18n("Resume Scheduler"));
}

void Scheduler::setPaused()
{
    pauseB->setCheckable(true);
    pauseB->setChecked(true);
    TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_NOTHING).toLatin1().data());
    moduleState()->setupNextIteration(RUN_NOTHING);
    appendLogText(i18n("Scheduler paused."));
}

void Scheduler::setActiveJob(SchedulerJob *job)
{
    // ignore setting the same active job twice
    if (activeJob() == job)
        return;

    /* Set current job */
    moduleState()->setActiveJob(job);

    /* Select the active job's row */
    if (activeJob())
    {
        int index = moduleState()->jobs().indexOf(job);

        // select the row only if editing is not ongoing
        if (index >= 0 && jobUnderEdit < 0)
        {
            queueTable->selectRow(index);
            syncGUIToJob(job);
        }
    }
    else
    {
        jobStatus->setText(i18n("No job running"));
    }
}

void Scheduler::syncGreedyParams()
{
    m_GreedyScheduler->setParams(
        errorHandlingRestartImmediatelyButton->isChecked(),
        errorHandlingRestartQueueButton->isChecked(),
        errorHandlingRescheduleErrorsCB->isChecked(),
        errorHandlingStrategyDelay->value(),
        errorHandlingStrategyDelay->value());
}

void Scheduler::evaluateJobs(bool evaluateOnly)
{
    for (auto job : moduleState()->jobs())
        job->clearCache();

    /* Don't evaluate if list is empty */
    if (moduleState()->jobs().isEmpty())
        return;
    /* Start by refreshing the number of captures already present - unneeded if not remembering job progress */
    if (Options::rememberJobProgress())
        updateCompletedJobsCount();

    moduleState()->calculateDawnDusk();

    syncGreedyParams();
    m_GreedyScheduler->scheduleJobs(moduleState()->jobs(), SchedulerModuleState::getLocalTime(),
                                    moduleState()->capturedFramesCount(), this);
    // schedule or job states might have been changed, update the table
    updateJobTable();

    if (!evaluateOnly && moduleState()->schedulerState() == SCHEDULER_RUNNING)
        // At this step, we finished evaluating jobs.
        // We select the first job that has to be run, per schedule.
        selectActiveJob(moduleState()->jobs());
    else
        qCInfo(KSTARS_EKOS_SCHEDULER) << "Ekos finished evaluating jobs, no job selection required.";

    emit jobsUpdated(getJSONJobs());
}

void Scheduler::selectActiveJob(const QList<SchedulerJob *> &jobs)
{
    auto finished_or_aborted = [](SchedulerJob const * const job)
    {
        SchedulerJobStatus const s = job->getState();
        return SCHEDJOB_ERROR <= s || SCHEDJOB_ABORTED == s;
    };

    /* This predicate matches jobs that are neither scheduled to run nor aborted */
    auto neither_scheduled_nor_aborted = [](SchedulerJob const * const job)
    {
        SchedulerJobStatus const s = job->getState();
        return SCHEDJOB_SCHEDULED != s && SCHEDJOB_ABORTED != s;
    };

    /* If there are no jobs left to run in the filtered list, stop evaluation */
    if (jobs.isEmpty() || std::all_of(jobs.begin(), jobs.end(), neither_scheduled_nor_aborted))
    {
        appendLogText(i18n("No jobs left in the scheduler queue after evaluating."));
        setActiveJob(nullptr);
        return;
    }
    /* If there are only aborted jobs that can run, reschedule those and let Scheduler restart one loop */
    else if (std::all_of(jobs.begin(), jobs.end(), finished_or_aborted) &&
             errorHandlingDontRestartButton->isChecked() == false)
    {
        appendLogText(i18n("Only aborted jobs left in the scheduler queue after evaluating, rescheduling those."));
        std::for_each(jobs.begin(), jobs.end(), [](SchedulerJob * job)
        {
            if (SCHEDJOB_ABORTED == job->getState())
                job->setState(SCHEDJOB_EVALUATION);
        });

        return;
    }

    // GreedyScheduler::scheduleJobs() must be called first.
    SchedulerJob *scheduledJob = m_GreedyScheduler->getScheduledJob();
    if (!scheduledJob)
    {
        appendLogText(i18n("No jobs scheduled."));
        setActiveJob(nullptr);
        return;
    }
    setActiveJob(scheduledJob);
}

void Scheduler::wakeUpScheduler()
{
    sleepLabel->hide();

    if (moduleState()->preemptiveShutdown())
    {
        moduleState()->disablePreemptiveShutdown();
        appendLogText(i18n("Scheduler is awake."));
        execute();
    }
    else
    {
        if (moduleState()->schedulerState() == SCHEDULER_RUNNING)
            appendLogText(i18n("Scheduler is awake. Jobs shall be started when ready..."));
        else
            appendLogText(i18n("Scheduler is awake. Jobs shall be started when scheduler is resumed."));

        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SCHEDULER);
    }
}

bool Scheduler::executeJob(SchedulerJob *job)
{
    // Some states have executeJob called after current job is cancelled - checkStatus does this
    if (job == nullptr)
        return false;

    // Don't execute the current job if it is already busy
    if (activeJob() == job && SCHEDJOB_BUSY == activeJob()->getState())
        return false;

    setActiveJob(job);

    // If we already started, we check when the next object is scheduled at.
    // If it is more than 30 minutes in the future, we park the mount if that is supported
    // and we unpark when it is due to start.
    //int const nextObservationTime = now.secsTo(getActiveJob()->getStartupTime());

    // If the time to wait is greater than the lead time (5 minutes by default)
    // then we sleep, otherwise we wait. It's the same thing, just different labels.
    if (shouldSchedulerSleep(activeJob()))
        return false;
    // If job schedule isn't now, wait - continuing to execute would cancel a parking attempt
    else if (0 < SchedulerModuleState::getLocalTime().secsTo(activeJob()->getStartupTime()))
        return false;

    // From this point job can be executed now

    if (job->getCompletionCondition() == FINISH_SEQUENCE && Options::rememberJobProgress())
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s%s\n", __LINE__, "captureInterface:setProperty", "targetName=",
                   job->getName().toLatin1().data());
        process()->captureInterface()->setProperty("targetName", job->getName());
    }

    moduleState()->calculateDawnDusk();
    updateNightTime();

    // Reset autofocus so that focus step is applied properly when checked
    // When the focus step is not checked, the capture module will eventually run focus periodically
    moduleState()->setAutofocusCompleted(false);

    qCInfo(KSTARS_EKOS_SCHEDULER) << "Executing Job " << activeJob()->getName();

    activeJob()->setState(SCHEDJOB_BUSY);
    emit jobsUpdated(getJSONJobs());

    KSNotification::event(QLatin1String("EkosSchedulerJobStart"),
                          i18n("Ekos job started (%1)", activeJob()->getName()), KSNotification::Scheduler);

    // No need to continue evaluating jobs as we already have one.
    TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_JOBCHECK).toLatin1().data());
    moduleState()->setupNextIteration(RUN_JOBCHECK);
    return true;
}

bool Scheduler::checkShutdownState()
{
    if (moduleState()->schedulerState() == SCHEDULER_PAUSED)
        return false;

    if (moduleState()->shutdownState() == SHUTDOWN_IDLE)
    {
        KSNotification::event(QLatin1String("ObservatoryShutdown"), i18n("Observatory is in the shutdown process"),
                              KSNotification::Scheduler);

        qCInfo(KSTARS_EKOS_SCHEDULER) << "Starting shutdown process...";

        //            weatherTimer.stop();
        //            weatherTimer.disconnect();
        weatherLabel->hide();

        setActiveJob(nullptr);

        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SHUTDOWN).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SHUTDOWN);

    }

    return process()->checkShutdownState();
}

bool Scheduler::checkStatus()
{
    if (moduleState()->schedulerState() == SCHEDULER_PAUSED)
    {
        if (activeJob() == nullptr)
        {
            setPaused();
            return false;
        }
        switch (activeJob()->getState())
        {
            case  SCHEDJOB_BUSY:
                // do nothing
                break;
            case  SCHEDJOB_COMPLETE:
                // start finding next job before pausing
                break;
            default:
                // in all other cases pause
                setPaused();
                break;
        }
    }

    // #1 If no current job selected, let's check if we need to shutdown or evaluate jobs
    if (activeJob() == nullptr)
    {
        // #2.1 If shutdown is already complete or in error, we need to stop
        if (moduleState()->shutdownState() == SHUTDOWN_COMPLETE
                || moduleState()->shutdownState() == SHUTDOWN_ERROR)
        {
            return process()->completeShutdown();
        }

        // #2.2  Check if shutdown is in progress
        if (moduleState()->shutdownState() > SHUTDOWN_IDLE)
        {
            // If Ekos is not done stopping, try again later
            if (moduleState()->ekosState() == EKOS_STOPPING && process()->checkEkosState() == false)
                return false;

            checkShutdownState();
            return false;
        }

        // #2.3 Check if park wait procedure is in progress
        if (process()->checkParkWaitState() == false)
            return false;

        // #2.4 If not in shutdown state, evaluate the jobs
        evaluateJobs(false);

        // #2.5 check if all jobs have completed and repeat is set
        if (nullptr == activeJob() && checkRepeatSequence())
        {
            // Reset all jobs
            resetJobs();
            // Re-evaluate all jobs to check whether there is at least one that might be executed
            evaluateJobs(false);
            // if there is an executable job, restart;
            if (activeJob())
            {
                sequenceExecutionCounter++;
                appendLogText(i18n("Starting job sequence iteration #%1", sequenceExecutionCounter));
                return true;
            }
        }

        // #2.6 If there is no current job after evaluation, shutdown
        if (nullptr == activeJob())
        {
            checkShutdownState();
            return false;
        }
    }
    // JM 2018-12-07: Check if we need to sleep
    else if (shouldSchedulerSleep(activeJob()) == false)
    {
        // #3 Check if startup procedure has failed.
        if (moduleState()->startupState() == STARTUP_ERROR)
        {
            // Stop Scheduler
            stop();
            return true;
        }

        // #4 Check if startup procedure Phase #1 is complete (Startup script)
        if ((moduleState()->startupState() == STARTUP_IDLE
                && process()->checkStartupState() == false)
                || moduleState()->startupState() == STARTUP_SCRIPT)
            return false;

        // #5 Check if Ekos is started
        if (process()->checkEkosState() == false)
            return false;

        // #6 Check if INDI devices are connected.
        if (process()->checkINDIState() == false)
            return false;

        // #6.1 Check if park wait procedure is in progress - in the case we're waiting for a distant job
        if (process()->checkParkWaitState() == false)
            return false;

        // #7 Check if startup procedure Phase #2 is complete (Unparking phase)
        if (moduleState()->startupState() > STARTUP_SCRIPT
                && moduleState()->startupState() < STARTUP_ERROR
                && process()->checkStartupState() == false)
            return false;

        // #8 Check it it already completed (should only happen starting a paused job)
        //    Find the next job in this case, otherwise execute the current one
        if (activeJob() && activeJob()->getState() == SCHEDJOB_COMPLETE)
            findNextJob();

        // N.B. We explicitly do not check for return result here because regardless of execution result
        // we do not have any pending tasks further down.
        executeJob(activeJob());
        updateJobTable();
    }

    return true;
}

void Scheduler::checkJobStage()
{
    Q_ASSERT_X(activeJob(), __FUNCTION__, "Actual current job is required to check job stage");
    if (!activeJob())
        return;

    if (checkJobStageCounter == 0)
    {
        qCDebug(KSTARS_EKOS_SCHEDULER) << "Checking job stage for" << activeJob()->getName() << "startup" <<
                                       activeJob()->getStartupCondition() << activeJob()->getStartupTime().toString(
                                           startupTimeEdit->displayFormat()) << "state" << activeJob()->getState();
        if (checkJobStageCounter++ == 30)
            checkJobStageCounter = 0;
    }


    syncGreedyParams();
    if (!m_GreedyScheduler->checkJob(moduleState()->jobs(), SchedulerModuleState::getLocalTime(), activeJob()))
    {
        activeJob()->setState(SCHEDJOB_IDLE);
        stopCurrentJobAction();
        findNextJob();
        return;
    }
    checkJobStageEplogue();
}

void Scheduler::checkJobStageEplogue()
{
    if (!activeJob())
        return;

    // #5 Check system status to improve robustness
    // This handles external events such as disconnections or end-user manipulating INDI panel
    if (!checkStatus())
        return;

    // #5b Check the guiding timer, and possibly restart guiding.
    process()->processGuidingTimer();

    // #6 Check each stage is processing properly
    // FIXME: Vanishing property should trigger a call to its event callback
    if (!activeJob()) return;
    switch (activeJob()->getStage())
    {
        case SCHEDSTAGE_IDLE:
            // Job is just starting.
            emit jobStarted(activeJob()->getName());
            getNextAction();
            break;

        case SCHEDSTAGE_ALIGNING:
            // Let's make sure align module does not become unresponsive
            if (moduleState()->getCurrentOperationMsec() > static_cast<int>(ALIGN_INACTIVITY_TIMEOUT))
            {
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "alignInterface:property", "status");
                QVariant const status = process()->alignInterface()->property("status");
                TEST_PRINT(stderr, "  @@@dbus received %d\n", !status.isValid() ? -1 : status.toInt());
                Ekos::AlignState alignStatus = static_cast<Ekos::AlignState>(status.toInt());

                if (alignStatus == Ekos::ALIGN_IDLE)
                {
                    if (moduleState()->increaseAlignFailureCount())
                    {
                        qCDebug(KSTARS_EKOS_SCHEDULER) << "Align module timed out. Restarting request...";
                        process()->startAstrometry();
                    }
                    else
                    {
                        appendLogText(i18n("Warning: job '%1' alignment procedure failed, marking aborted.", activeJob()->getName()));
                        activeJob()->setState(SCHEDJOB_ABORTED);
                        findNextJob();
                    }
                }
                else
                    moduleState()->startCurrentOperationTimer();
            }
            break;

        case SCHEDSTAGE_CAPTURING:
            // Let's make sure capture module does not become unresponsive
            if (moduleState()->getCurrentOperationMsec() > static_cast<int>(CAPTURE_INACTIVITY_TIMEOUT))
            {
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "captureInterface:property", "status");
                QVariant const status = process()->captureInterface()->property("status");
                TEST_PRINT(stderr, "  @@@dbus received %d\n", !status.isValid() ? -1 : status.toInt());
                Ekos::CaptureState captureStatus = static_cast<Ekos::CaptureState>(status.toInt());

                if (captureStatus == Ekos::CAPTURE_IDLE)
                {
                    if (moduleState()->increaseCaptureFailureCount())
                    {
                        qCDebug(KSTARS_EKOS_SCHEDULER) << "capture module timed out. Restarting request...";
                        process()->startCapture();
                    }
                    else
                    {
                        appendLogText(i18n("Warning: job '%1' capture procedure failed, marking aborted.", activeJob()->getName()));
                        activeJob()->setState(SCHEDJOB_ABORTED);
                        findNextJob();
                    }
                }
                else moduleState()->startCurrentOperationTimer();
            }
            break;

        case SCHEDSTAGE_FOCUSING:
            // Let's make sure focus module does not become unresponsive
            if (moduleState()->getCurrentOperationMsec() > static_cast<int>(FOCUS_INACTIVITY_TIMEOUT))
            {
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "focusInterface:property", "status");
                QVariant const status = process()->focusInterface()->property("status");
                TEST_PRINT(stderr, "  @@@dbus received %d\n", !status.isValid() ? -1 : status.toInt());
                Ekos::FocusState focusStatus = static_cast<Ekos::FocusState>(status.toInt());

                if (focusStatus == Ekos::FOCUS_IDLE || focusStatus == Ekos::FOCUS_WAITING)
                {
                    if (moduleState()->increaseFocusFailureCount())
                    {
                        qCDebug(KSTARS_EKOS_SCHEDULER) << "Focus module timed out. Restarting request...";
                        process()->startFocusing();
                    }
                    else
                    {
                        appendLogText(i18n("Warning: job '%1' focusing procedure failed, marking aborted.", activeJob()->getName()));
                        activeJob()->setState(SCHEDJOB_ABORTED);
                        findNextJob();
                    }
                }
                else moduleState()->startCurrentOperationTimer();
            }
            break;

        case SCHEDSTAGE_GUIDING:
            // Let's make sure guide module does not become unresponsive
            if (moduleState()->getCurrentOperationMsec() > GUIDE_INACTIVITY_TIMEOUT)
            {
                GuideState guideStatus = process()->getGuidingStatus();

                if (guideStatus == Ekos::GUIDE_IDLE || guideStatus == Ekos::GUIDE_CONNECTED || guideStatus == Ekos::GUIDE_DISCONNECTED)
                {
                    if (moduleState()->increaseGuideFailureCount())
                    {
                        qCDebug(KSTARS_EKOS_SCHEDULER) << "guide module timed out. Restarting request...";
                        process()->startGuiding();
                    }
                    else
                    {
                        appendLogText(i18n("Warning: job '%1' guiding procedure failed, marking aborted.", activeJob()->getName()));
                        activeJob()->setState(SCHEDJOB_ABORTED);
                        findNextJob();
                    }
                }
                else moduleState()->startCurrentOperationTimer();
            }
            break;

        case SCHEDSTAGE_SLEWING:
        case SCHEDSTAGE_RESLEWING:
            // While slewing or re-slewing, check slew status can still be obtained
        {
            TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "mountInterface:property", "status");
            QVariant const slewStatus = process()->mountInterface()->property("status");
            TEST_PRINT(stderr, "  @@@dbus received %d\n", !slewStatus.isValid() ? -1 : slewStatus.toInt());

            if (slewStatus.isValid())
            {
                // Send the slew status periodically to avoid the situation where the mount is already at location and does not send any event
                // FIXME: in that case, filter TRACKING events only?
                ISD::Mount::Status const status = static_cast<ISD::Mount::Status>(slewStatus.toInt());
                process()->setMountStatus(status);
            }
            else
            {
                appendLogText(i18n("Warning: job '%1' lost connection to the mount, attempting to reconnect.", activeJob()->getName()));
                if (!process()->manageConnectionLoss())
                    activeJob()->setState(SCHEDJOB_ERROR);
                return;
            }
        }
        break;

        case SCHEDSTAGE_SLEW_COMPLETE:
        case SCHEDSTAGE_RESLEWING_COMPLETE:
            // When done slewing or re-slewing and we use a dome, only shift to the next action when the dome is done moving
            if (moduleState()->domeReady())
            {
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "domeInterface:property", "isMoving");
                QVariant const isDomeMoving = process()->domeInterface()->property("isMoving");
                TEST_PRINT(stderr, "  @@@dbus received %s\n",
                           !isDomeMoving.isValid() ? "invalid" : (isDomeMoving.value<bool>() ? "T" : "F"));

                if (!isDomeMoving.isValid())
                {
                    appendLogText(i18n("Warning: job '%1' lost connection to the dome, attempting to reconnect.", activeJob()->getName()));
                    if (!process()->manageConnectionLoss())
                        activeJob()->setState(SCHEDJOB_ERROR);
                    return;
                }

                if (!isDomeMoving.value<bool>())
                    getNextAction();
            }
            else getNextAction();
            break;

        default:
            break;
    }
}

void Scheduler::getNextAction()
{
    qCDebug(KSTARS_EKOS_SCHEDULER) << "Get next action...";

    switch (activeJob()->getStage())
    {
        case SCHEDSTAGE_IDLE:
            if (activeJob()->getLightFramesRequired())
            {
                if (activeJob()->getStepPipeline() & SchedulerJob::USE_TRACK)
                    process()->startSlew();
                else if (activeJob()->getStepPipeline() & SchedulerJob::USE_FOCUS && moduleState()->autofocusCompleted() == false)
                {
                    qCDebug(KSTARS_EKOS_SCHEDULER) << "process()->startFocusing on 3485";
                    process()->startFocusing();
                }
                else if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN)
                    process()->startAstrometry();
                else if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                    if (process()->getGuidingStatus() == GUIDE_GUIDING)
                    {
                        appendLogText(i18n("Guiding already running, directly start capturing."));
                        process()->startCapture();
                    }
                    else
                        process()->startGuiding();
                else
                    process()->startCapture();
            }
            else
            {
                if (activeJob()->getStepPipeline())
                    appendLogText(
                        i18n("Job '%1' is proceeding directly to capture stage because only calibration frames are pending.",
                             activeJob()->getName()));
                process()->startCapture();
            }

            break;

        case SCHEDSTAGE_SLEW_COMPLETE:
            if (activeJob()->getStepPipeline() & SchedulerJob::USE_FOCUS && moduleState()->autofocusCompleted() == false)
            {
                qCDebug(KSTARS_EKOS_SCHEDULER) << "process()->startFocusing on 3514";
                process()->startFocusing();
            }
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN)
                process()->startAstrometry();
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                process()->startGuiding();
            else
                process()->startCapture();
            break;

        case SCHEDSTAGE_FOCUS_COMPLETE:
            if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN)
                process()->startAstrometry();
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                process()->startGuiding();
            else
                process()->startCapture();
            break;

        case SCHEDSTAGE_ALIGN_COMPLETE:
            updateJobStage(SCHEDSTAGE_RESLEWING);
            break;

        case SCHEDSTAGE_RESLEWING_COMPLETE:
            // If we have in-sequence-focus in the sequence file then we perform post alignment focusing so that the focus
            // frame is ready for the capture module in-sequence-focus procedure.
            if ((activeJob()->getStepPipeline() & SchedulerJob::USE_FOCUS) && activeJob()->getInSequenceFocus())
                // Post alignment re-focusing
            {
                qCDebug(KSTARS_EKOS_SCHEDULER) << "process()->startFocusing on 3544";
                process()->startFocusing();
            }
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                process()->startGuiding();
            else
                process()->startCapture();
            break;

        case SCHEDSTAGE_POSTALIGN_FOCUSING_COMPLETE:
            if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                process()->startGuiding();
            else
                process()->startCapture();
            break;

        case SCHEDSTAGE_GUIDING_COMPLETE:
            process()->startCapture();
            break;

        default:
            break;
    }
}

void Scheduler::stopCurrentJobAction()
{
    if (nullptr != activeJob())
    {
        qCDebug(KSTARS_EKOS_SCHEDULER) << "Job '" << activeJob()->getName() << "' is stopping current action..." <<
                                       activeJob()->getStage();

        switch (activeJob()->getStage())
        {
            case SCHEDSTAGE_IDLE:
                break;

            case SCHEDSTAGE_SLEWING:
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "mountInterface:call", "abort");
                process()->mountInterface()->call(QDBus::AutoDetect, "abort");
                break;

            case SCHEDSTAGE_FOCUSING:
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "focusInterface:call", "abort");
                process()->focusInterface()->call(QDBus::AutoDetect, "abort");
                break;

            case SCHEDSTAGE_ALIGNING:
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "alignInterface:call", "abort");
                process()->alignInterface()->call(QDBus::AutoDetect, "abort");
                break;

            // N.B. Need to use BlockWithGui as proposed by Wolfgang
            // to ensure capture is properly aborted before taking any further actions.
            case SCHEDSTAGE_CAPTURING:
                TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "captureInterface:call", "abort");
                process()->captureInterface()->call(QDBus::BlockWithGui, "abort");
                break;

            default:
                break;
        }

        /* Reset interrupted job stage */
        updateJobStage(SCHEDSTAGE_IDLE);
    }

    /* Guiding being a parallel process, check to stop it */
    process()->stopGuiding();
}

void Scheduler::load(bool clearQueue, const QString &filename)
{
    QUrl fileURL;

    if (filename.isEmpty())
        fileURL = QFileDialog::getOpenFileUrl(Ekos::Manager::Instance(), i18nc("@title:window", "Open Ekos Scheduler List"),
                                              dirPath,
                                              "Ekos Scheduler List (*.esl)");
    else fileURL.setUrl(filename);

    if (fileURL.isEmpty())
        return;

    if (fileURL.isValid() == false)
    {
        QString message = i18n("Invalid URL: %1", fileURL.toLocalFile());
        KSNotification::sorry(message, i18n("Invalid URL"));
        return;
    }

    dirPath = QUrl(fileURL.url(QUrl::RemoveFilename));

    if (clearQueue)
        removeAllJobs();
    // remember toe number of rows to select the first one appended
    const int row = moduleState()->jobs().count();

    // try appending the jobs from the file to the job list
    if (process()->appendEkosScheduleList(fileURL.toLocalFile()))
    {
        // select the first appended row (if any was added)
        if (moduleState()->jobs().count() > row)
            queueTable->selectRow(row);

        /* Run a job idle evaluation after a successful load */
        startJobEvaluation();
    }
}

void Scheduler::removeAllJobs()
{
    if (jobUnderEdit >= 0)
        resetJobEdit();

    while (queueTable->rowCount() > 0)
        queueTable->removeRow(0);

    qDeleteAll(moduleState()->jobs());
    moduleState()->mutlableJobs().clear();
}

bool Scheduler::loadScheduler(const QString &fileURL)
{
    removeAllJobs();
    return process()->appendEkosScheduleList(fileURL);
}

void Scheduler::saveAs()
{
    schedulerURL.clear();
    save();
}

void Scheduler::save()
{
    QUrl backupCurrent = schedulerURL;

    if (schedulerURL.toLocalFile().startsWith(QLatin1String("/tmp/")) || schedulerURL.toLocalFile().contains("/Temp"))
        schedulerURL.clear();

    // If no changes made, return.
    if (moduleState()->dirty() == false && !schedulerURL.isEmpty())
        return;

    if (schedulerURL.isEmpty())
    {
        schedulerURL =
            QFileDialog::getSaveFileUrl(Ekos::Manager::Instance(), i18nc("@title:window", "Save Ekos Scheduler List"), dirPath,
                                        "Ekos Scheduler List (*.esl)");
        // if user presses cancel
        if (schedulerURL.isEmpty())
        {
            schedulerURL = backupCurrent;
            return;
        }

        dirPath = QUrl(schedulerURL.url(QUrl::RemoveFilename));

        if (schedulerURL.toLocalFile().contains('.') == 0)
            schedulerURL.setPath(schedulerURL.toLocalFile() + ".esl");
    }

    if (schedulerURL.isValid())
    {
        if ((saveScheduler(schedulerURL)) == false)
        {
            KSNotification::error(i18n("Failed to save scheduler list"), i18n("Save"));
            return;
        }

        // update save button tool tip
        queueSaveB->setToolTip("Save schedule to " + schedulerURL.fileName());
    }
    else
    {
        QString message = i18n("Invalid URL: %1", schedulerURL.url());
        KSNotification::sorry(message, i18n("Invalid URL"));
    }
}

bool Scheduler::canCountCaptures(const SchedulerJob &job)
{
    QList<SequenceJob*> seqjobs;
    bool hasAutoFocus = false;
    SchedulerJob tempJob = job;
    if (SchedulerUtils::loadSequenceQueue(tempJob.getSequenceFile().toLocalFile(), &tempJob, seqjobs, hasAutoFocus,
                                          nullptr) == false)
        return false;

    for (const SequenceJob *oneSeqJob : seqjobs)
    {
        if (oneSeqJob->getUploadMode() == ISD::Camera::UPLOAD_LOCAL)
            return false;
    }
    return true;
}

// FindNextJob (probably misnamed) deals with what to do when jobs end.
// For instance, if they complete their capture sequence, they may
// (a) be done, (b) be part of a repeat N times, or (c) be part of a loop forever.
// Similarly, if jobs are aborted they may (a) restart right away, (b) restart after a delay, (c) be ended.
void Scheduler::findNextJob()
{
    if (moduleState()->schedulerState() == SCHEDULER_PAUSED)
    {
        // everything finished, we can pause
        setPaused();
        return;
    }

    Q_ASSERT_X(activeJob()->getState() == SCHEDJOB_ERROR ||
               activeJob()->getState() == SCHEDJOB_ABORTED ||
               activeJob()->getState() == SCHEDJOB_COMPLETE ||
               activeJob()->getState() == SCHEDJOB_IDLE,
               __FUNCTION__, "Finding next job requires current to be in error, aborted, idle or complete");

    // Reset failed count
    moduleState()->resetAlignFailureCount();
    moduleState()->resetGuideFailureCount();
    moduleState()->resetFocusFailureCount();
    moduleState()->resetCaptureFailureCount();

    if (activeJob()->getState() == SCHEDJOB_ERROR || activeJob()->getState() == SCHEDJOB_ABORTED)
    {
        emit jobEnded(activeJob()->getName(), activeJob()->getStopReason());
        moduleState()->resetCaptureBatch();
        // Stop Guiding if it was used
        process()->stopGuiding();

        if (activeJob()->getState() == SCHEDJOB_ERROR)
            appendLogText(i18n("Job '%1' is terminated due to errors.", activeJob()->getName()));
        else
            appendLogText(i18n("Job '%1' is aborted.", activeJob()->getName()));

        // Always reset job stage
        updateJobStage(SCHEDSTAGE_IDLE);

        // restart aborted jobs immediately, if error handling strategy is set to "restart immediately"
        if (errorHandlingRestartImmediatelyButton->isChecked() &&
                (activeJob()->getState() == SCHEDJOB_ABORTED ||
                 (activeJob()->getState() == SCHEDJOB_ERROR && errorHandlingRescheduleErrorsCB->isChecked())))
        {
            // reset the state so that it will be restarted
            activeJob()->setState(SCHEDJOB_SCHEDULED);

            appendLogText(i18n("Waiting %1 seconds to restart job '%2'.", errorHandlingStrategyDelay->value(), activeJob()->getName()));

            // wait the given delay until the jobs will be evaluated again
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_WAKEUP).toLatin1().data());
            moduleState()->setupNextIteration(RUN_WAKEUP, std::lround((errorHandlingStrategyDelay->value() * 1000) /
                                              KStarsData::Instance()->clock()->scale()));
            sleepLabel->setToolTip(i18n("Scheduler waits for a retry."));
            sleepLabel->show();
            return;
        }

        // otherwise start re-evaluation
        setActiveJob(nullptr);
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SCHEDULER);
    }
    else if (activeJob()->getState() == SCHEDJOB_IDLE)
    {
        emit jobEnded(activeJob()->getName(), activeJob()->getStopReason());

        // job constraints no longer valid, start re-evaluation
        setActiveJob(nullptr);
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SCHEDULER);
    }
    // Job is complete, so check completion criteria to optimize processing
    // In any case, we're done whether the job completed successfully or not.
    else if (activeJob()->getCompletionCondition() == FINISH_SEQUENCE)
    {
        emit jobEnded(activeJob()->getName(), activeJob()->getStopReason());

        /* If we remember job progress, mark the job idle as well as all its duplicates for re-evaluation */
        if (Options::rememberJobProgress())
        {
            foreach(SchedulerJob *a_job, moduleState()->jobs())
                if (a_job == activeJob() || a_job->isDuplicateOf(activeJob()))
                    a_job->setState(SCHEDJOB_IDLE);
        }

        moduleState()->resetCaptureBatch();
        // Stop Guiding if it was used
        process()->stopGuiding();

        appendLogText(i18n("Job '%1' is complete.", activeJob()->getName()));

        // Always reset job stage
        updateJobStage(SCHEDSTAGE_IDLE);

        // If saving remotely, then can't tell later that the job has been completed.
        // Set it complete now.
        if (!canCountCaptures(*activeJob()))
            activeJob()->setState(SCHEDJOB_COMPLETE);

        setActiveJob(nullptr);
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SCHEDULER);
    }
    else if (activeJob()->getCompletionCondition() == FINISH_REPEAT &&
             (activeJob()->getRepeatsRemaining() <= 1))
    {
        /* If the job is about to repeat, decrease its repeat count and reset its start time */
        if (activeJob()->getRepeatsRemaining() > 0)
        {
            // If we can remember job progress, this is done in estimateJobTime()
            if (!Options::rememberJobProgress())
            {
                activeJob()->setRepeatsRemaining(activeJob()->getRepeatsRemaining() - 1);
                activeJob()->setCompletedIterations(activeJob()->getCompletedIterations() + 1);
            }
            activeJob()->setStartupTime(QDateTime());
        }

        /* Mark the job idle as well as all its duplicates for re-evaluation */
        foreach(SchedulerJob *a_job, moduleState()->jobs())
            if (a_job == activeJob() || a_job->isDuplicateOf(activeJob()))
                a_job->setState(SCHEDJOB_IDLE);

        /* Re-evaluate all jobs, without selecting a new job */
        evaluateJobs(true);

        /* If current job is actually complete because of previous duplicates, prepare for next job */
        if (activeJob() == nullptr || activeJob()->getRepeatsRemaining() == 0)
        {
            stopCurrentJobAction();

            if (activeJob() != nullptr)
            {
                emit jobEnded(activeJob()->getName(), activeJob()->getStopReason());
                appendLogText(i18np("Job '%1' is complete after #%2 batch.",
                                    "Job '%1' is complete after #%2 batches.",
                                    activeJob()->getName(), activeJob()->getRepeatsRequired()));
                if (!canCountCaptures(*activeJob()))
                    activeJob()->setState(SCHEDJOB_COMPLETE);
                setActiveJob(nullptr);
            }
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
            moduleState()->setupNextIteration(RUN_SCHEDULER);
        }
        /* If job requires more work, continue current observation */
        else
        {
            /* FIXME: raise priority to allow other jobs to schedule in-between */
            if (executeJob(activeJob()) == false)
                return;

            /* JM 2020-08-23: If user opts to force realign instead of for each job then we force this FIRST */
            if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN && Options::forceAlignmentBeforeJob())
            {
                process()->stopGuiding();
                updateJobStage(SCHEDSTAGE_ALIGNING);
                process()->startAstrometry();
            }
            /* If we are guiding, continue capturing */
            else if ( (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE) )
            {
                updateJobStage(SCHEDSTAGE_CAPTURING);
                process()->startCapture();
            }
            /* If we are not guiding, but using alignment, realign */
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN)
            {
                updateJobStage(SCHEDSTAGE_ALIGNING);
                process()->startAstrometry();
            }
            /* Else if we are neither guiding nor using alignment, slew back to target */
            else if (activeJob()->getStepPipeline() & SchedulerJob::USE_TRACK)
            {
                updateJobStage(SCHEDSTAGE_SLEWING);
                process()->startSlew();
            }
            /* Else just start capturing */
            else
            {
                updateJobStage(SCHEDSTAGE_CAPTURING);
                process()->startCapture();
            }

            appendLogText(i18np("Job '%1' is repeating, #%2 batch remaining.",
                                "Job '%1' is repeating, #%2 batches remaining.",
                                activeJob()->getName(), activeJob()->getRepeatsRemaining()));
            /* getActiveJob() remains the same */
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_JOBCHECK).toLatin1().data());
            moduleState()->setupNextIteration(RUN_JOBCHECK);
        }
    }
    else if ((activeJob()->getCompletionCondition() == FINISH_LOOP) ||
             (activeJob()->getCompletionCondition() == FINISH_REPEAT &&
              activeJob()->getRepeatsRemaining() > 0))
    {
        /* If the job is about to repeat, decrease its repeat count and reset its start time */
        if ((activeJob()->getCompletionCondition() == FINISH_REPEAT) &&
                (activeJob()->getRepeatsRemaining() > 1))
        {
            // If we can remember job progress, this is done in estimateJobTime()
            if (!Options::rememberJobProgress())
            {
                activeJob()->setRepeatsRemaining(activeJob()->getRepeatsRemaining() - 1);
                activeJob()->setCompletedIterations(activeJob()->getCompletedIterations() + 1);
            }
            activeJob()->setStartupTime(QDateTime());
        }

        if (executeJob(activeJob()) == false)
            return;

        if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN && Options::forceAlignmentBeforeJob())
        {
            process()->stopGuiding();
            updateJobStage(SCHEDSTAGE_ALIGNING);
            process()->startAstrometry();
        }
        else
        {
            updateJobStage(SCHEDSTAGE_CAPTURING);
            process()->startCapture();
        }

        moduleState()->increaseCaptureBatch();

        if (activeJob()->getCompletionCondition() == FINISH_REPEAT )
            appendLogText(i18np("Job '%1' is repeating, #%2 batch remaining.",
                                "Job '%1' is repeating, #%2 batches remaining.",
                                activeJob()->getName(), activeJob()->getRepeatsRemaining()));
        else
            appendLogText(i18n("Job '%1' is repeating, looping indefinitely.", activeJob()->getName()));

        /* getActiveJob() remains the same */
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_JOBCHECK).toLatin1().data());
        moduleState()->setupNextIteration(RUN_JOBCHECK);
    }
    else if (activeJob()->getCompletionCondition() == FINISH_AT)
    {
        if (SchedulerModuleState::getLocalTime().secsTo(activeJob()->getCompletionTime()) <= 0)
        {
            emit jobEnded(activeJob()->getName(), activeJob()->getStopReason());

            /* Mark the job idle as well as all its duplicates for re-evaluation */
            foreach(SchedulerJob *a_job, moduleState()->jobs())
                if (a_job == activeJob() || a_job->isDuplicateOf(activeJob()))
                    a_job->setState(SCHEDJOB_IDLE);
            stopCurrentJobAction();

            moduleState()->resetCaptureBatch();

            appendLogText(i18np("Job '%1' stopping, reached completion time with #%2 batch done.",
                                "Job '%1' stopping, reached completion time with #%2 batches done.",
                                activeJob()->getName(), moduleState()->captureBatch() + 1));

            // Always reset job stage
            updateJobStage(SCHEDSTAGE_IDLE);

            setActiveJob(nullptr);
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
            moduleState()->setupNextIteration(RUN_SCHEDULER);
        }
        else
        {
            if (executeJob(activeJob()) == false)
                return;

            if (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN && Options::forceAlignmentBeforeJob())
            {
                process()->stopGuiding();
                updateJobStage(SCHEDSTAGE_ALIGNING);
                process()->startAstrometry();
            }
            else
            {
                updateJobStage(SCHEDSTAGE_CAPTURING);
                process()->startCapture();
            }

            moduleState()->increaseCaptureBatch();

            appendLogText(i18np("Job '%1' completed #%2 batch before completion time, restarted.",
                                "Job '%1' completed #%2 batches before completion time, restarted.",
                                activeJob()->getName(), moduleState()->captureBatch()));
            /* getActiveJob() remains the same */
            TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_JOBCHECK).toLatin1().data());
            moduleState()->setupNextIteration(RUN_JOBCHECK);
        }
    }
    else
    {
        /* Unexpected situation, mitigate by resetting the job and restarting the scheduler timer */
        qCDebug(KSTARS_EKOS_SCHEDULER) << "BUGBUG! Job '" << activeJob()->getName() <<
                                       "' timer elapsed, but no action to be taken.";

        // Always reset job stage
        updateJobStage(SCHEDSTAGE_IDLE);

        setActiveJob(nullptr);
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
        moduleState()->setupNextIteration(RUN_SCHEDULER);
    }
}

void Scheduler::setDirty()
{
    // ignore changes that are a result of syncGUIToJob() or syncGUIToGeneralSettings()
    if (jobUnderEdit < 0)
        return;

    moduleState()->setDirty(true);

    if (sender() == startupProcedureButtonGroup || sender() == shutdownProcedureGroup)
        return;

    // update state
    if (sender() == schedulerStartupScript)
        moduleState()->setStartupScriptURL(QUrl::fromUserInput(schedulerStartupScript->text()));
    else if (sender() == schedulerShutdownScript)
        moduleState()->setShutdownScriptURL(QUrl::fromUserInput(schedulerShutdownScript->text()));

    // For object selection, all fields must be filled
    bool const nameSelectionOK = !raBox->isEmpty()  && !decBox->isEmpty() && !nameEdit->text().isEmpty();

    // For FITS selection, only the name and fits URL should be filled.
    bool const fitsSelectionOK = !nameEdit->text().isEmpty() && !fitsURL.isEmpty();

    // Sequence selection is required
    bool const seqSelectionOK = !sequenceEdit->text().isEmpty();

    // Finally, adding is allowed upon object/FITS and sequence selection
    bool const addingOK = (nameSelectionOK || fitsSelectionOK) && seqSelectionOK;

    addToQueueB->setEnabled(addingOK);
    //mosaicB->setEnabled(addingOK);
}

void Scheduler::updateCompletedJobsCount(bool forced)
{
    /* Use a temporary map in order to limit the number of file searches */
    CapturedFramesMap newFramesCount;

    /* FIXME: Capture storage cache is refreshed too often, feature requires rework. */

    /* Check if one job is idle or requires evaluation - if so, force refresh */
    forced |= std::any_of(moduleState()->jobs().begin(),
                          moduleState()->jobs().end(), [](SchedulerJob * oneJob) -> bool
    {
        SchedulerJobStatus const state = oneJob->getState();
        return state == SCHEDJOB_IDLE || state == SCHEDJOB_EVALUATION;});

    /* If update is forced, clear the frame map */
    if (forced)
        moduleState()->capturedFramesCount().clear();

    /* Enumerate SchedulerJobs to count captures that are already stored */
    for (SchedulerJob *oneJob : moduleState()->jobs())
    {
        QList<SequenceJob*> seqjobs;
        bool hasAutoFocus = false;

        //oneJob->setLightFramesRequired(false);
        /* Look into the sequence requirements, bypass if invalid */
        if (SchedulerUtils::loadSequenceQueue(oneJob->getSequenceFile().toLocalFile(), oneJob, seqjobs, hasAutoFocus,
                                              this) == false)
        {
            appendLogText(i18n("Warning: job '%1' has inaccessible sequence '%2', marking invalid.", oneJob->getName(),
                               oneJob->getSequenceFile().toLocalFile()));
            oneJob->setState(SCHEDJOB_INVALID);
            continue;
        }

        /* Enumerate the SchedulerJob's SequenceJobs to count captures stored for each */
        for (SequenceJob *oneSeqJob : seqjobs)
        {
            /* Only consider captures stored on client (Ekos) side */
            /* FIXME: ask the remote for the file count */
            if (oneSeqJob->getUploadMode() == ISD::Camera::UPLOAD_LOCAL)
                continue;

            /* FIXME: this signature path is incoherent when there is no filter wheel on the setup - bugfix should be elsewhere though */
            QString const signature = oneSeqJob->getSignature();

            /* If signature was processed during this run, keep it */
            if (newFramesCount.constEnd() != newFramesCount.constFind(signature))
                continue;

            /* If signature was processed during an earlier run, use the earlier count */
            QMap<QString, uint16_t>::const_iterator const earlierRunIterator = moduleState()->capturedFramesCount().constFind(
                        signature);
            if (moduleState()->capturedFramesCount().constEnd() != earlierRunIterator)
            {
                newFramesCount[signature] = earlierRunIterator.value();
                continue;
            }

            /* Else recount captures already stored */
            newFramesCount[signature] = PlaceholderPath::getCompletedFiles(signature);
        }

        // determine whether we need to continue capturing, depending on captured frames
        SchedulerUtils::updateLightFramesRequired(oneJob, seqjobs, newFramesCount);
    }

    moduleState()->setCapturedFramesCount(newFramesCount);

    {
        qCDebug(KSTARS_EKOS_SCHEDULER) << "Frame map summary:";
        QMap<QString, uint16_t>::const_iterator it = moduleState()->capturedFramesCount().constBegin();
        for (; it != moduleState()->capturedFramesCount().constEnd(); it++)
            qCDebug(KSTARS_EKOS_SCHEDULER) << " " << it.key() << ':' << it.value();
    }
}

void Scheduler::startJobEvaluation()
{
    // Reset all jobs
    // other states too?
    if (SCHEDULER_RUNNING != moduleState()->schedulerState())
        resetJobs();

    // reset the iterations counter
    sequenceExecutionCounter = 1;

    // And evaluate all pending jobs per the conditions set in each
    evaluateJobs(true);
}

void Ekos::Scheduler::resetJobs()
{
    setActiveJob(nullptr);

    // Reset ALL scheduler jobs to IDLE and force-reset their completed count - no effect when progress is kept
    for (SchedulerJob * job : moduleState()->jobs())
    {
        job->reset();
        job->setCompletedCount(0);
    }

    // Unconditionally update the capture storage
    updateCompletedJobsCount(true);
}

void Scheduler::sortJobsPerAltitude()
{
    // We require a first job to sort, so bail out if list is empty
    if (moduleState()->jobs().isEmpty())
        return;

    // Don't reset current job
    // setCurrentJob(nullptr);

    // Don't reset scheduler jobs startup times before sorting - we need the first job startup time

    // Sort by startup time, using the first job time as reference for altitude calculations
    using namespace std::placeholders;
    QList<SchedulerJob*> sortedJobs = moduleState()->jobs();
    std::stable_sort(sortedJobs.begin() + 1, sortedJobs.end(),
                     std::bind(SchedulerJob::decreasingAltitudeOrder, _1, _2, moduleState()->jobs().first()->getStartupTime()));

    // If order changed, reset and re-evaluate
    if (reorderJobs(sortedJobs))
    {
        for (SchedulerJob * job : moduleState()->jobs())
            job->reset();

        evaluateJobs(true);
    }
}

void Scheduler::resumeCheckStatus()
{
    disconnect(this, &Scheduler::weatherChanged, this, &Scheduler::resumeCheckStatus);
    TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_SCHEDULER).toLatin1().data());
    moduleState()->setupNextIteration(RUN_SCHEDULER);
}

ErrorHandlingStrategy Scheduler::getErrorHandlingStrategy()
{
    // The UI holds the state
    if (errorHandlingRestartQueueButton->isChecked())
        return ERROR_RESTART_AFTER_TERMINATION;
    else if (errorHandlingRestartImmediatelyButton->isChecked())
        return ERROR_RESTART_IMMEDIATELY;
    else
        return ERROR_DONT_RESTART;
}

void Scheduler::setErrorHandlingStrategy(ErrorHandlingStrategy strategy)
{
    errorHandlingStrategyDelay->setEnabled(strategy != ERROR_DONT_RESTART);

    switch (strategy)
    {
        case ERROR_RESTART_AFTER_TERMINATION:
            errorHandlingRestartQueueButton->setChecked(true);
            break;
        case ERROR_RESTART_IMMEDIATELY:
            errorHandlingRestartImmediatelyButton->setChecked(true);
            break;
        default:
            errorHandlingDontRestartButton->setChecked(true);
            break;
    }
}

// Can't use a SchedulerAlgorithm type for the arg here
// as the compiler is unhappy connecting the signals currentIndexChanged(int)
// or activated(int) to an enum.
void Scheduler::setAlgorithm(int algIndex)
{
    if (algIndex != ALGORITHM_GREEDY)
    {
        appendLogText(i18n("Warning: The Classic scheduler algorithm has been retired. Switching you to the Greedy algorithm."));
        algIndex = ALGORITHM_GREEDY;
    }
    Options::setSchedulerAlgorithm(algIndex);

    groupLabel->setDisabled(false);
    groupEdit->setDisabled(false);
    queueTable->model()->setHeaderData(START_TIME_COLUMN, Qt::Horizontal, tr("Next Start"));
    queueTable->model()->setHeaderData(END_TIME_COLUMN, Qt::Horizontal, tr("Next End"));
    queueTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
}

void Scheduler::resetAllJobs()
{
    if (moduleState()->schedulerState() == SCHEDULER_RUNNING)
        return;

    // Reset capture count of all jobs before re-evaluating
    foreach (SchedulerJob *job, moduleState()->jobs())
        job->setCompletedCount(0);

    // Evaluate all jobs, this refreshes storage and resets job states
    startJobEvaluation();
}

void Scheduler::checkTwilightWarning(bool enabled)
{
    if (enabled)
        return;

    if (KMessageBox::warningContinueCancel(
                nullptr,
                i18n("Turning off astronomial twilight check may cause the observatory "
                     "to run during daylight. This can cause irreversible damage to your equipment!"),
                i18n("Astronomial Twilight Warning"), KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
                "astronomical_twilight_warning") == KMessageBox::Cancel)
    {
        schedulerTwilight->setChecked(true);
    }
}


void Scheduler::updateProfiles()
{
    schedulerProfileCombo->blockSignals(true);
    schedulerProfileCombo->clear();
    schedulerProfileCombo->addItems(moduleState()->profiles());
    schedulerProfileCombo->setCurrentText(moduleState()->currentProfile());
    schedulerProfileCombo->blockSignals(false);
}

void Scheduler::updateJobStage(SchedulerJobStage stage)
{
    updateJobStageUI(stage);

    if (activeJob()->getStage() != stage)
        activeJob()->setStage(stage);
}

void Scheduler::updateJobStageUI(SchedulerJobStage stage)
{
    /* Translated string cache - overkill, probably, and doesn't warn about missing enums like switch/case should ; also, not thread-safe */
    /* FIXME: this should work with a static initializer in C++11, but QT versions are touchy on this, and perhaps i18n can't be used? */
    static QMap<SchedulerJobStage, QString> stageStrings;
    static QString stageStringUnknown;
    if (stageStrings.isEmpty())
    {
        stageStrings[SCHEDSTAGE_IDLE] = i18n("Idle");
        stageStrings[SCHEDSTAGE_SLEWING] = i18n("Slewing");
        stageStrings[SCHEDSTAGE_SLEW_COMPLETE] = i18n("Slew complete");
        stageStrings[SCHEDSTAGE_FOCUSING] =
            stageStrings[SCHEDSTAGE_POSTALIGN_FOCUSING] = i18n("Focusing");
        stageStrings[SCHEDSTAGE_FOCUS_COMPLETE] =
            stageStrings[SCHEDSTAGE_POSTALIGN_FOCUSING_COMPLETE ] = i18n("Focus complete");
        stageStrings[SCHEDSTAGE_ALIGNING] = i18n("Aligning");
        stageStrings[SCHEDSTAGE_ALIGN_COMPLETE] = i18n("Align complete");
        stageStrings[SCHEDSTAGE_RESLEWING] = i18n("Repositioning");
        stageStrings[SCHEDSTAGE_RESLEWING_COMPLETE] = i18n("Repositioning complete");
        /*stageStrings[SCHEDSTAGE_CALIBRATING] = i18n("Calibrating");*/
        stageStrings[SCHEDSTAGE_GUIDING] = i18n("Guiding");
        stageStrings[SCHEDSTAGE_GUIDING_COMPLETE] = i18n("Guiding complete");
        stageStrings[SCHEDSTAGE_CAPTURING] = i18n("Capturing");
        stageStringUnknown = i18n("Unknown");
    }

    if (activeJob() == nullptr)
        jobStatus->setText(stageStrings[SCHEDSTAGE_IDLE]);
    else
        jobStatus->setText(QString("%1: %2").arg(activeJob()->getName(),
                                                 stageStrings.value(stage, stageStringUnknown)));

}


void Scheduler::setINDICommunicationStatus(Ekos::CommunicationStatus status)
{
    TEST_PRINT(stderr, "sch%d @@@dbus(%s): %d\n", __LINE__, "ekosInterface:indiStatusChanged", status);
    qCDebug(KSTARS_EKOS_SCHEDULER) << "Scheduler INDI status is" << status;

    moduleState()->setIndiCommunicationStatus(status);
}

void Scheduler::setEkosCommunicationStatus(Ekos::CommunicationStatus status)
{
    TEST_PRINT(stderr, "sch%d @@@dbus(%s): %d\n", __LINE__, "ekosInterface:ekosStatusChanged", status);
    qCDebug(KSTARS_EKOS_SCHEDULER) << "Scheduler Ekos status is" << status;

    moduleState()->setEkosCommunicationStatus(status);
}

void Scheduler::simClockScaleChanged(float newScale)
{
    if (currentlySleeping())
    {
        QTime const remainingTimeMs = QTime::fromMSecsSinceStartOfDay(std::lround(static_cast<double>
                                      (moduleState()->iterationTimer().remainingTime())
                                      * KStarsData::Instance()->clock()->scale()
                                      / newScale));
        appendLogText(i18n("Sleeping for %1 on simulation clock update until next observation job is ready...",
                           remainingTimeMs.toString("hh:mm:ss")));
        moduleState()->iterationTimer().stop();
        moduleState()->iterationTimer().start(remainingTimeMs.msecsSinceStartOfDay());
    }
}

void Scheduler::simClockTimeChanged()
{
    moduleState()->calculateDawnDusk();
    updateNightTime();

    // If the Scheduler is not running, reset all jobs and re-evaluate from a new current start point
    if (SCHEDULER_RUNNING != moduleState()->schedulerState())
    {
        startJobEvaluation();
    }
}

void Scheduler::registerNewDevice(const QString &name, int interface)
{
    Q_UNUSED(name)

    if (interface & INDI::BaseDevice::DOME_INTERFACE)
    {
        QList<QVariant> dbusargs;
        dbusargs.append(INDI::BaseDevice::DOME_INTERFACE);
        QDBusReply<QStringList> paths = process()->indiInterface()->callWithArgumentList(QDBus::AutoDetect, "getDevicesPaths",
                                        dbusargs);
        if (paths.error().type() == QDBusError::NoError)
        {
            // Select last device in case a restarted caused multiple instances in the tree
            setDomePathString(paths.value().last());
            delete process()->domeInterface();
            process()->setDomeInterface(new QDBusInterface(kstarsInterfaceString, domePathString, domeInterfaceString,
                                        QDBusConnection::sessionBus(), this));
            connect(process()->domeInterface(), SIGNAL(ready()), this, SLOT(syncProperties()));
            checkInterfaceReady(process()->domeInterface());
        }
    }

    if (interface & INDI::BaseDevice::WEATHER_INTERFACE)
    {
        QList<QVariant> dbusargs;
        dbusargs.append(INDI::BaseDevice::WEATHER_INTERFACE);
        QDBusReply<QStringList> paths = process()->indiInterface()->callWithArgumentList(QDBus::AutoDetect, "getDevicesPaths",
                                        dbusargs);
        if (paths.error().type() == QDBusError::NoError)
        {
            // Select last device in case a restarted caused multiple instances in the tree
            setWeatherPathString(paths.value().last());
            delete process()->weatherInterface();
            process()->setWeatherInterface(new QDBusInterface(kstarsInterfaceString, weatherPathString, weatherInterfaceString,
                                           QDBusConnection::sessionBus(), this));
            connect(process()->weatherInterface(), SIGNAL(ready()), this, SLOT(syncProperties()));
            connect(process()->weatherInterface(), SIGNAL(newStatus(ISD::Weather::Status)), this,
                    SLOT(setWeatherStatus(ISD::Weather::Status)));
            checkInterfaceReady(process()->weatherInterface());
        }
    }

    if (interface & INDI::BaseDevice::DUSTCAP_INTERFACE)
    {
        QList<QVariant> dbusargs;
        dbusargs.append(INDI::BaseDevice::DUSTCAP_INTERFACE);
        QDBusReply<QStringList> paths = process()->indiInterface()->callWithArgumentList(QDBus::AutoDetect, "getDevicesPaths",
                                        dbusargs);
        if (paths.error().type() == QDBusError::NoError)
        {
            // Select last device in case a restarted caused multiple instances in the tree
            setDustCapPathString(paths.value().last());
            delete process()->capInterface();
            process()->setCapInterface(new QDBusInterface(kstarsInterfaceString, dustCapPathString, dustCapInterfaceString,
                                       QDBusConnection::sessionBus(), this));
            connect(process()->capInterface(), SIGNAL(ready()), this, SLOT(syncProperties()));
            checkInterfaceReady(process()->capInterface());
        }
    }
}

void Scheduler::registerNewModule(const QString &name)
{
    qCDebug(KSTARS_EKOS_SCHEDULER) << "Registering new Module (" << name << ")";

    if (name == "Focus")
    {
        delete process()->focusInterface();
        process()->setFocusInterface(new QDBusInterface(kstarsInterfaceString, focusPathString, focusInterfaceString,
                                     QDBusConnection::sessionBus(), this));
        connect(process()->focusInterface(), SIGNAL(newStatus(Ekos::FocusState)), this,
                SLOT(setFocusStatus(Ekos::FocusState)), Qt::UniqueConnection);
    }
    else if (name == "Capture")
    {
        delete process()->captureInterface();
        process()->setCaptureInterface(new QDBusInterface(kstarsInterfaceString, capturePathString, captureInterfaceString,
                                       QDBusConnection::sessionBus(), this));

        connect(process()->captureInterface(), SIGNAL(ready()), this, SLOT(syncProperties()));
        connect(process()->captureInterface(), SIGNAL(newStatus(Ekos::CaptureState)), this,
                SLOT(setCaptureStatus(Ekos::CaptureState)),
                Qt::UniqueConnection);
        connect(process()->captureInterface(), SIGNAL(captureComplete(QVariantMap)), this, SLOT(setCaptureComplete(QVariantMap)),
                Qt::UniqueConnection);
        checkInterfaceReady(process()->captureInterface());
    }
    else if (name == "Mount")
    {
        delete process()->mountInterface();
        process()->setMountInterface(new QDBusInterface(kstarsInterfaceString, mountPathString, mountInterfaceString,
                                     QDBusConnection::sessionBus(), this));

        connect(process()->mountInterface(), SIGNAL(ready()), this, SLOT(syncProperties()));
        connect(process()->mountInterface(), SIGNAL(newStatus(ISD::Mount::Status)), this, SLOT(setMountStatus(ISD::Mount::Status)),
                Qt::UniqueConnection);

        checkInterfaceReady(process()->mountInterface());
    }
    else if (name == "Align")
    {
        delete process()->alignInterface();
        process()->setAlignInterface(new QDBusInterface(kstarsInterfaceString, alignPathString, alignInterfaceString,
                                     QDBusConnection::sessionBus(), this));
        connect(process()->alignInterface(), SIGNAL(newStatus(Ekos::AlignState)), this, SLOT(setAlignStatus(Ekos::AlignState)),
                Qt::UniqueConnection);
    }
    else if (name == "Guide")
    {
        delete process()->guideInterface();
        process()->setGuideInterface(new QDBusInterface(kstarsInterfaceString, guidePathString, guideInterfaceString,
                                     QDBusConnection::sessionBus(), this));
        connect(process()->guideInterface(), SIGNAL(newStatus(Ekos::GuideState)), this,
                SLOT(setGuideStatus(Ekos::GuideState)), Qt::UniqueConnection);
    }
}

void Scheduler::syncProperties()
{
    QDBusInterface *iface = qobject_cast<QDBusInterface*>(sender());

    if (iface == process()->mountInterface())
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "mountInterface:property", "canPark");
        QVariant canMountPark = process()->mountInterface()->property("canPark");
        TEST_PRINT(stderr, "  @@@dbus received %s\n", !canMountPark.isValid() ? "invalid" : (canMountPark.toBool() ? "T" : "F"));

        schedulerUnparkMount->setEnabled(canMountPark.toBool());
        schedulerParkMount->setEnabled(canMountPark.toBool());
        moduleState()->setMountReady(true);
    }
    else if (iface == process()->capInterface())
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "dustCapInterface:property", "canPark");
        QVariant canCapPark = process()->capInterface()->property("canPark");
        TEST_PRINT(stderr, "  @@@dbus received %s\n", !canCapPark.isValid() ? "invalid" : (canCapPark.toBool() ? "T" : "F"));

        if (canCapPark.isValid())
        {
            schedulerCloseDustCover->setEnabled(canCapPark.toBool());
            schedulerOpenDustCover->setEnabled(canCapPark.toBool());
            moduleState()->setCapReady(true);
        }
        else
        {
            schedulerCloseDustCover->setEnabled(false);
            schedulerOpenDustCover->setEnabled(false);
        }
    }
    else if (iface == process()->domeInterface())
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "domeInterface:property", "canPark");
        QVariant canDomePark = process()->domeInterface()->property("canPark");
        TEST_PRINT(stderr, "  @@@dbus received %s\n", !canDomePark.isValid() ? "invalid" : (canDomePark.toBool() ? "T" : "F"));

        if (canDomePark.isValid())
        {
            schedulerParkDome->setEnabled(canDomePark.toBool());
            schedulerUnparkDome->setEnabled(canDomePark.toBool());
            moduleState()->setDomeReady(true);
        }
        else
        {
            schedulerParkDome->setEnabled(false);
            schedulerUnparkDome->setEnabled(false);
        }
    }
    else if (iface == process()->weatherInterface())
    {
        QVariant status = process()->weatherInterface()->property("status");
        if (status.isValid())
        {
            schedulerWeather->setEnabled(true);
            setWeatherStatus(static_cast<ISD::Weather::Status>(status.toInt()));
        }
        else
            schedulerWeather->setEnabled(false);
    }
    else if (iface == process()->captureInterface())
    {
        TEST_PRINT(stderr, "sch%d @@@dbus(%s): %s\n", __LINE__, "captureInterface:property", "coolerControl");
        QVariant hasCoolerControl = process()->captureInterface()->property("coolerControl");
        TEST_PRINT(stderr, "  @@@dbus received %s\n",
                   !hasCoolerControl.isValid() ? "invalid" : (hasCoolerControl.toBool() ? "T" : "F"));
        schedulerWarmCCD->setEnabled(hasCoolerControl.toBool());
        moduleState()->setCaptureReady(true);
    }
}

void Scheduler::checkInterfaceReady(QDBusInterface *iface)
{
    if (iface == process()->mountInterface())
    {
        QVariant canMountPark = process()->mountInterface()->property("canPark");
        if (canMountPark.isValid())
        {
            schedulerUnparkMount->setEnabled(canMountPark.toBool());
            schedulerParkMount->setEnabled(canMountPark.toBool());
            moduleState()->setMountReady(true);
        }
    }
    else if (iface == process()->capInterface())
    {
        QVariant canCapPark = process()->capInterface()->property("canPark");
        if (canCapPark.isValid())
        {
            schedulerCloseDustCover->setEnabled(canCapPark.toBool());
            schedulerOpenDustCover->setEnabled(canCapPark.toBool());
            moduleState()->setCapReady(true);
        }
        else
        {
            schedulerCloseDustCover->setEnabled(false);
            schedulerOpenDustCover->setEnabled(false);
        }
    }
    else if (iface == process()->weatherInterface())
    {
        QVariant status = process()->weatherInterface()->property("status");
        if (status.isValid())
        {
            schedulerWeather->setEnabled(true);
            setWeatherStatus(static_cast<ISD::Weather::Status>(status.toInt()));
        }
        else
            schedulerWeather->setEnabled(false);
    }
    else if (iface == process()->domeInterface())
    {
        QVariant canDomePark = process()->domeInterface()->property("canPark");
        if (canDomePark.isValid())
        {
            schedulerUnparkDome->setEnabled(canDomePark.toBool());
            schedulerParkDome->setEnabled(canDomePark.toBool());
            moduleState()->setDomeReady(true);
        }
    }
    else if (iface == process()->captureInterface())
    {
        QVariant hasCoolerControl = process()->captureInterface()->property("coolerControl");
        if (hasCoolerControl.isValid())
        {
            schedulerWarmCCD->setEnabled(hasCoolerControl.toBool());
            moduleState()->setCaptureReady(true);
        }
    }
}

void Scheduler::setAlignStatus(AlignState status)
{
    process()->setAlignStatus(status);
}

void Scheduler::setGuideStatus(GuideState status)
{
    process()->setGuideStatus(status);
}

void Scheduler::setCaptureStatus(Ekos::CaptureState status)
{
    TEST_PRINT(stderr, "sch%d @@@setCaptureStatus(%d) %s\n", __LINE__, static_cast<int>(status),
               (activeJob() == nullptr) ? "IGNORED" : "OK");
    if (activeJob() == nullptr)
        return;

    qCDebug(KSTARS_EKOS_SCHEDULER) << "Capture State" << Ekos::getCaptureStatusString(status);

    /* If current job is scheduled and has not started yet, wait */
    if (SCHEDJOB_SCHEDULED == activeJob()->getState())
    {
        QDateTime const now = SchedulerModuleState::getLocalTime();
        if (now < activeJob()->getStartupTime())
            return;
    }

    if (activeJob()->getStage() == SCHEDSTAGE_CAPTURING)
    {
        if (status == Ekos::CAPTURE_PROGRESS && (activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN))
        {
            // JM 2021.09.20
            // Re-set target coords in align module
            // When capture starts, alignment module automatically rests target coords to mount coords.
            // However, we want to keep align module target synced with the scheduler target and not
            // the mount coord
            const SkyPoint targetCoords = activeJob()->getTargetCoords();
            QList<QVariant> targetArgs;
            targetArgs << targetCoords.ra0().Hours() << targetCoords.dec0().Degrees();
            process()->alignInterface()->callWithArgumentList(QDBus::AutoDetect, "setTargetCoords", targetArgs);
        }
        else if (status == Ekos::CAPTURE_ABORTED)
        {
            appendLogText(i18n("Warning: job '%1' failed to capture target.", activeJob()->getName()));

            if (moduleState()->increaseCaptureFailureCount())
            {
                // If capture failed due to guiding error, let's try to restart that
                if (activeJob()->getStepPipeline() & SchedulerJob::USE_GUIDE)
                {
                    // Check if it is guiding related.
                    Ekos::GuideState gStatus = process()->getGuidingStatus();
                    if (gStatus == Ekos::GUIDE_ABORTED ||
                            gStatus == Ekos::GUIDE_CALIBRATION_ERROR ||
                            gStatus == GUIDE_DITHERING_ERROR)
                    {
                        appendLogText(i18n("Job '%1' is capturing, is restarting its guiding procedure (attempt #%2 of %3).",
                                           activeJob()->getName(),
                                           moduleState()->captureFailureCount(), moduleState()->maxFailureAttempts()));
                        process()->startGuiding(true);
                        return;
                    }
                }

                /* FIXME: it's not clear whether it is actually possible to continue capturing when capture fails this way */
                appendLogText(i18n("Warning: job '%1' failed its capture procedure, restarting capture.", activeJob()->getName()));
                process()->startCapture(true);
            }
            else
            {
                /* FIXME: it's not clear whether this situation can be recovered at all */
                appendLogText(i18n("Warning: job '%1' failed its capture procedure, marking aborted.", activeJob()->getName()));
                activeJob()->setState(SCHEDJOB_ABORTED);

                findNextJob();
            }
        }
        else if (status == Ekos::CAPTURE_COMPLETE)
        {
            KSNotification::event(QLatin1String("EkosScheduledImagingFinished"),
                                  i18n("Ekos job (%1) - Capture finished", activeJob()->getName()), KSNotification::Scheduler);

            activeJob()->setState(SCHEDJOB_COMPLETE);
            findNextJob();
        }
        else if (status == Ekos::CAPTURE_IMAGE_RECEIVED)
        {
            // We received a new image, but we don't know precisely where so update the storage map and re-estimate job times.
            // FIXME: rework this once capture storage is reworked
            if (Options::rememberJobProgress())
            {
                updateCompletedJobsCount(true);

                for (const auto &job : moduleState()->jobs())
                    SchedulerUtils::estimateJobTime(job, moduleState()->capturedFramesCount(), this);
            }
            // Else if we don't remember the progress on jobs, increase the completed count for the current job only - no cross-checks
            else
                activeJob()->setCompletedCount(activeJob()->getCompletedCount() + 1);

            moduleState()->resetCaptureFailureCount();
        }
    }
}

void Scheduler::setFocusStatus(FocusState status)
{
    process()->setFocusStatus(status);
}

void Scheduler::setMountStatus(ISD::Mount::Status status)
{
    process()->setMountStatus(status);
}

void Scheduler::setWeatherStatus(ISD::Weather::Status status)
{
    TEST_PRINT(stderr, "sch%d @@@setWeatherStatus(%d)\n", __LINE__, static_cast<int>(status));
    ISD::Weather::Status newStatus = status;
    QString statusString;

    switch (newStatus)
    {
        case ISD::Weather::WEATHER_OK:
            statusString = i18n("Weather conditions are OK.");
            break;

        case ISD::Weather::WEATHER_WARNING:
            statusString = i18n("Warning: weather conditions are in the WARNING zone.");
            break;

        case ISD::Weather::WEATHER_ALERT:
            statusString = i18n("Caution: weather conditions are in the DANGER zone!");
            break;

        default:
            break;
    }

    if (newStatus != moduleState()->weatherStatus())
    {
        moduleState()->setWeatherStatus(newStatus);

        qCDebug(KSTARS_EKOS_SCHEDULER) << statusString;

        if (moduleState()->weatherStatus() == ISD::Weather::WEATHER_OK)
            weatherLabel->setPixmap(
                QIcon::fromTheme("security-high")
                .pixmap(QSize(32, 32)));
        else if (moduleState()->weatherStatus() == ISD::Weather::WEATHER_WARNING)
        {
            weatherLabel->setPixmap(
                QIcon::fromTheme("security-medium")
                .pixmap(QSize(32, 32)));
            KSNotification::event(QLatin1String("WeatherWarning"), i18n("Weather conditions in warning zone"),
                                  KSNotification::Scheduler, KSNotification::Warn);
        }
        else if (moduleState()->weatherStatus() == ISD::Weather::WEATHER_ALERT)
        {
            weatherLabel->setPixmap(
                QIcon::fromTheme("security-low")
                .pixmap(QSize(32, 32)));
            KSNotification::event(QLatin1String("WeatherAlert"),
                                  i18n("Weather conditions are critical. Observatory shutdown is imminent"), KSNotification::Scheduler,
                                  KSNotification::Alert);
        }
        else
            weatherLabel->setPixmap(QIcon::fromTheme("chronometer")
                                    .pixmap(QSize(32, 32)));

        weatherLabel->show();
        weatherLabel->setToolTip(statusString);

        appendLogText(statusString);

        emit weatherChanged(moduleState()->weatherStatus());
    }

    // Shutdown scheduler if it was started and not already in shutdown
    // and if weather checkbox is checked.
    if (schedulerWeather->isChecked() && moduleState()->weatherStatus() == ISD::Weather::WEATHER_ALERT
            && moduleState()->schedulerState() != Ekos::SCHEDULER_IDLE
            && moduleState()->schedulerState() != Ekos::SCHEDULER_SHUTDOWN)
    {
        appendLogText(i18n("Starting shutdown procedure due to severe weather."));
        if (activeJob())
        {
            activeJob()->setState(SCHEDJOB_ABORTED);
            stopCurrentJobAction();
        }
        checkShutdownState();
    }
}

bool Scheduler::shouldSchedulerSleep(SchedulerJob *job)
{
    Q_ASSERT_X(nullptr != job, __FUNCTION__,
               "There must be a valid current job for Scheduler to test sleep requirement");

    if (job->getLightFramesRequired() == false)
        return false;

    QDateTime const now = SchedulerModuleState::getLocalTime();
    int const nextObservationTime = now.secsTo(job->getStartupTime());

    // It is possible that the nextObservationTime is far away, but the reason is that
    // the user has edited the jobs, and now the active job is not the next thing scheduled.
    syncGreedyParams();
    if (m_GreedyScheduler->getScheduledJob() != job)
        return false;

    // If start up procedure is complete and the user selected pre-emptive shutdown, let us check if the next observation time exceed
    // the pre-emptive shutdown time in hours (default 2). If it exceeds that, we perform complete shutdown until next job is ready
    if (moduleState()->startupState() == STARTUP_COMPLETE &&
            Options::preemptiveShutdown() &&
            nextObservationTime > (Options::preemptiveShutdownTime() * 3600))
    {
        appendLogText(i18n(
                          "Job '%1' scheduled for execution at %2. "
                          "Observatory scheduled for shutdown until next job is ready.",
                          job->getName(), job->getStartupTime().toString(startupTimeEdit->displayFormat())));
        moduleState()->enablePreemptiveShutdown(job->getStartupTime());
        schedulerWeather->setEnabled(false);
        weatherLabel->hide();
        checkShutdownState();
        return true;
    }
    // Otherwise, sleep until job is ready
    /* FIXME: if not parking, stop tracking maybe? this would prevent crashes or scheduler stops from leaving the mount to track and bump the pier */
    // If start up procedure is already complete, and we didn't issue any parking commands before and parking is checked and enabled
    // Then we park the mount until next job is ready. But only if the job uses TRACK as its first step, otherwise we cannot get into position again.
    // This is also only performed if next job is due more than the default lead time (5 minutes).
    // If job is due sooner than that is not worth parking and we simply go into sleep or wait modes.
    else if (nextObservationTime > Options::leadTime() * 60 &&
             moduleState()->startupState() == STARTUP_COMPLETE &&
             moduleState()->parkWaitState() == PARKWAIT_IDLE &&
             (job->getStepPipeline() & SchedulerJob::USE_TRACK) &&
             schedulerParkMount->isEnabled() &&
             schedulerParkMount->isChecked())
    {
        appendLogText(i18n(
                          "Job '%1' scheduled for execution at %2. "
                          "Parking the mount until the job is ready.",
                          job->getName(), job->getStartupTime().toString()));

        moduleState()->setParkWaitState(PARKWAIT_PARK);

        return false;
    }
    else if (nextObservationTime > Options::leadTime() * 60)
    {
        appendLogText(i18n("Sleeping until observation job %1 is ready at %2...", job->getName(),
                           now.addSecs(nextObservationTime + 1).toString()));
        sleepLabel->setToolTip(i18n("Scheduler is in sleep mode"));
        sleepLabel->show();

        // Warn the user if the next job is really far away - 60/5 = 12 times the lead time
        if (nextObservationTime > Options::leadTime() * 60 * 12 && !Options::preemptiveShutdown())
        {
            dms delay(static_cast<double>(nextObservationTime * 15.0 / 3600.0));
            appendLogText(i18n(
                              "Warning: Job '%1' is %2 away from now, you may want to enable Preemptive Shutdown.",
                              job->getName(), delay.toHMSString()));
        }

        /* FIXME: stop tracking now */

        // Wake up when job is due.
        // FIXME: Implement waking up periodically before job is due for weather check.
        // int const nextWakeup = nextObservationTime < 60 ? nextObservationTime : 60;
        TEST_PRINT(stderr, "%d Setting %s\n", __LINE__, timerStr(RUN_WAKEUP).toLatin1().data());
        moduleState()->setupNextIteration(RUN_WAKEUP,
                                          std::lround(((nextObservationTime + 1) * 1000) / KStarsData::Instance()->clock()->scale()));

        return true;
    }

    return false;
}

void Scheduler::setCaptureComplete(const QVariantMap &metadata)
{
    if (activeJob() &&
            activeJob()->getStepPipeline() & SchedulerJob::USE_ALIGN &&
            metadata["type"].toInt() == FRAME_LIGHT &&
            Options::alignCheckFrequency() > 0 &&
            ++m_SolverIteration >= Options::alignCheckFrequency())
    {
        m_SolverIteration = 0;

        auto filename = metadata["filename"].toString();
        auto exposure = metadata["exposure"].toDouble();

        constexpr double minSolverSeconds = 5.0;
        double solverTimeout = std::max(exposure - 2, minSolverSeconds);
        if (solverTimeout >= minSolverSeconds)
        {
            auto profiles = getDefaultAlignOptionsProfiles();
            auto parameters = profiles.at(Options::solveOptionsProfile());
            // Double search radius
            parameters.search_radius = parameters.search_radius * 2;
            m_Solver.reset(new SolverUtils(parameters, solverTimeout),  &QObject::deleteLater);
            connect(m_Solver.get(), &SolverUtils::done, this, &Ekos::Scheduler::solverDone, Qt::UniqueConnection);
            //connect(m_Solver.get(), &SolverUtils::newLog, this, &Ekos::Scheduler::appendLogText, Qt::UniqueConnection);

            auto width = metadata["width"].toUInt();
            auto height = metadata["height"].toUInt();

            auto lowScale = Options::astrometryImageScaleLow();
            auto highScale = Options::astrometryImageScaleHigh();

            // solver utils uses arcsecs per pixel only
            if (Options::astrometryImageScaleUnits() == SSolver::DEG_WIDTH)
            {
                lowScale = (lowScale * 3600) / std::max(width, height);
                highScale = (highScale * 3600) / std::min(width, height);
            }
            else if (Options::astrometryImageScaleUnits() == SSolver::ARCMIN_WIDTH)
            {
                lowScale = (lowScale * 60) / std::max(width, height);
                highScale = (highScale * 60) / std::min(width, height);
            }

            m_Solver->useScale(Options::astrometryUseImageScale(), lowScale, highScale);
            m_Solver->usePosition(Options::astrometryUsePosition(), activeJob()->getTargetCoords().ra().Degrees(),
                                  activeJob()->getTargetCoords().dec().Degrees());
            m_Solver->setHealpix(moduleState()->indexToUse(), moduleState()->healpixToUse());
            m_Solver->runSolver(filename);
        }
    }
}

void Scheduler::solverDone(bool timedOut, bool success, const FITSImage::Solution &solution, double elapsedSeconds)
{
    disconnect(m_Solver.get(), &SolverUtils::done, this, &Ekos::Scheduler::solverDone);

    if (!activeJob())
        return;

    QString healpixString = "";
    if (moduleState()->indexToUse() != -1 || moduleState()->healpixToUse() != -1)
        healpixString = QString("Healpix %1 Index %2").arg(moduleState()->healpixToUse()).arg(moduleState()->indexToUse());

    if (timedOut || !success)
    {
        // Don't use the previous index and healpix next time we solve.
        moduleState()->setIndexToUse(-1);
        moduleState()->setHealpixToUse(-1);
    }
    else
    {
        int index, healpix;
        // Get the index and healpix from the successful solve.
        m_Solver->getSolutionHealpix(&index, &healpix);
        moduleState()->setIndexToUse(index);
        moduleState()->setHealpixToUse(healpix);
    }

    if (timedOut)
        appendLogText(i18n("Solver timed out: %1s %2", QString("%L1").arg(elapsedSeconds, 0, 'f', 1), healpixString));
    else if (!success)
        appendLogText(i18n("Solver failed: %1s %2", QString("%L1").arg(elapsedSeconds, 0, 'f', 1), healpixString));
    else
    {
        const double ra = solution.ra;
        const double dec = solution.dec;

        const auto target = activeJob()->getTargetCoords();

        SkyPoint alignCoord;
        alignCoord.setRA0(ra / 15.0);
        alignCoord.setDec0(dec);
        alignCoord.apparentCoord(static_cast<long double>(J2000), KStars::Instance()->data()->ut().djd());
        alignCoord.EquatorialToHorizontal(KStarsData::Instance()->lst(), KStarsData::Instance()->geo()->lat());
        const double diffRa = (alignCoord.ra().deltaAngle(target.ra())).Degrees() * 3600;
        const double diffDec = (alignCoord.dec().deltaAngle(target.dec())).Degrees() * 3600;

        // This is an approximation, probably ok for small angles.
        const double diffTotal = hypot(diffRa, diffDec);

        // Note--the RA output is in DMS. This is because we're looking at differences in arcseconds
        // and HMS coordinates are misleading (one HMS second is really 6 arc-seconds).
        qCDebug(KSTARS_EKOS_SCHEDULER) <<
                                       QString("Target Distance: %1\" Target (RA: %2 DE: %3) Current (RA: %4 DE: %5) %6 solved in %7s")
                                       .arg(QString("%L1").arg(diffTotal, 0, 'f', 0),
                                            target.ra().toDMSString(),
                                            target.dec().toDMSString(),
                                            alignCoord.ra().toDMSString(),
                                            alignCoord.dec().toDMSString(),
                                            healpixString,
                                            QString("%L1").arg(elapsedSeconds, 0, 'f', 2));
        emit targetDistance(diffTotal);

        // If we exceed align check threshold, we abort and re-align.
        if (diffTotal / 60 > Options::alignCheckThreshold())
        {
            appendLogText(i18n("Captured frame is %1 arcminutes away from target, re-aligning...", QString::number(diffTotal / 60.0,
                               'f', 1)));
            stopCurrentJobAction();
            process()->startAstrometry();
        }
    }
}

QJsonArray Scheduler::getJSONJobs()
{
    QJsonArray jobArray;

    for (const auto &oneJob : moduleState()->jobs())
        jobArray.append(oneJob->toJson());

    return jobArray;
}

bool Scheduler::createJobSequence(XMLEle * root, const QString &prefix, const QString &outputDir)
{
    return process()->createJobSequence(root, prefix, outputDir);
}

XMLEle *Scheduler::getSequenceJobRoot(const QString &filename)
{
    return process()->getSequenceJobRoot(filename);
}

bool Scheduler::importMosaic(const QJsonObject &payload)
{
    QScopedPointer<FramingAssistantUI> assistant(new FramingAssistantUI());
    return assistant->importMosaic(payload);
}

void Scheduler::startupStateChanged(StartupState state)
{
    jobStatus->setText(startupStateString(state));

    switch (moduleState()->startupState())
    {
        case STARTUP_IDLE:
            startupB->setIcon(QIcon::fromTheme("media-playback-start"));
            break;
        case STARTUP_COMPLETE:
            startupB->setIcon(QIcon::fromTheme("media-playback-start"));
            appendLogText(i18n("Manual startup procedure completed successfully."));
            break;
        case STARTUP_ERROR:
            startupB->setIcon(QIcon::fromTheme("media-playback-start"));
            appendLogText(i18n("Manual startup procedure terminated due to errors."));
            break;
        default:
            // in all other cases startup is running
            startupB->setIcon(QIcon::fromTheme("media-playback-stop"));
            break;
    }
}
void Scheduler::shutdownStateChanged(ShutdownState state)
{
    jobStatus->setText(shutdownStateString(state));
    if (state == SHUTDOWN_COMPLETE || state == SHUTDOWN_IDLE
            || state == SHUTDOWN_ERROR)
        shutdownB->setIcon(QIcon::fromTheme("media-playback-start"));
    else
        shutdownB->setIcon(QIcon::fromTheme("media-playback-stop"));
}
void Scheduler::ekosStateChanged(EkosState state)
{
    jobStatus->setText(ekosStateString(state));
}
void Scheduler::indiStateChanged(INDIState state)
{
    jobStatus->setText(indiStateString(state));
}
void Scheduler::parkWaitStateChanged(ParkWaitState state)
{
    jobStatus->setText(parkWaitStateString(state));
}

SchedulerJob *Scheduler::activeJob()
{
    return moduleState()->activeJob();
}

Ekos::SchedulerState Scheduler::status()
{
    return moduleState()->schedulerState();
}

bool Scheduler::saveScheduler(const QUrl &fileURL)
{
    return process()->saveScheduler(fileURL);
}

void Scheduler::loadGlobalSettings()
{
    QString key;
    QVariant value;

    QVariantMap settings;
    // All Combo Boxes
    for (auto &oneWidget : findChildren<QComboBox*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setCurrentText(value.toString());
            settings[key] = value;
        }
        else
            qCDebug(KSTARS_EKOS_FOCUS) << "Option" << key << "not found!";
    }

    // All Double Spin Boxes
    for (auto &oneWidget : findChildren<QDoubleSpinBox*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setValue(value.toDouble());
            settings[key] = value;
        }
        else
            qCDebug(KSTARS_EKOS_FOCUS) << "Option" << key << "not found!";
    }

    // All Spin Boxes
    for (auto &oneWidget : findChildren<QSpinBox*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setValue(value.toInt());
            settings[key] = value;
        }
        else
            qCDebug(KSTARS_EKOS_FOCUS) << "Option" << key << "not found!";
    }

    // All Checkboxes
    for (auto &oneWidget : findChildren<QCheckBox*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setChecked(value.toBool());
            settings[key] = value;
        }
        else
            qCDebug(KSTARS_EKOS_FOCUS) << "Option" << key << "not found!";
    }

    // All Line Edits
    for (auto &oneWidget : findChildren<QLineEdit*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setText(value.toString());
            settings[key] = value;

            if (key == "sequenceEdit")
                setSequence(value.toString());
            else if (key == "schedulerStartupScript")
                moduleState()->setStartupScriptURL(QUrl::fromUserInput(value.toString()));
            else if (key == "schedulerShutdownScript")
                moduleState()->setShutdownScriptURL(QUrl::fromUserInput(value.toString()));
        }
        else
            qCDebug(KSTARS_EKOS_FOCUS) << "Option" << key << "not found!";
    }

    // All Radio buttons
    for (auto &oneWidget : findChildren<QRadioButton*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setChecked(value.toBool());
            settings[key] = value;
        }
    }

    // All QDateTime edits
    for (auto &oneWidget : findChildren<QDateTimeEdit*>())
    {
        key = oneWidget->objectName();
        value = Options::self()->property(key.toLatin1());
        if (value.isValid())
        {
            oneWidget->setDateTime(QDateTime::fromString(value.toString(), Qt::ISODate));
            settings[key] = value;
        }
    }

    setErrorHandlingStrategy(static_cast<ErrorHandlingStrategy>(Options::errorHandlingStrategy()));

    m_GlobalSettings = m_Settings = settings;
}

void Scheduler::syncSettings()
{
    QDoubleSpinBox *dsb = nullptr;
    QSpinBox *sb = nullptr;
    QCheckBox *cb = nullptr;
    QRadioButton *rb = nullptr;
    QComboBox *cbox = nullptr;
    QLineEdit *lineedit = nullptr;
    QDateTimeEdit *datetimeedit = nullptr;

    QString key;
    QVariant value;

    if ( (dsb = qobject_cast<QDoubleSpinBox*>(sender())))
    {
        key = dsb->objectName();
        value = dsb->value();

    }
    else if ( (sb = qobject_cast<QSpinBox*>(sender())))
    {
        key = sb->objectName();
        value = sb->value();
    }
    else if ( (cb = qobject_cast<QCheckBox*>(sender())))
    {
        key = cb->objectName();
        value = cb->isChecked();
    }
    else if ( (rb = qobject_cast<QRadioButton*>(sender())))
    {
        key = rb->objectName();
        value = rb->isChecked();
    }
    else if ( (cbox = qobject_cast<QComboBox*>(sender())))
    {
        key = cbox->objectName();
        value = cbox->currentText();
    }
    else if ( (lineedit = qobject_cast<QLineEdit*>(sender())))
    {
        key = lineedit->objectName();
        value = lineedit->text();
    }
    else if ( (datetimeedit = qobject_cast<QDateTimeEdit*>(sender())))
    {
        key = datetimeedit->objectName();
        value = datetimeedit->dateTime().toString(Qt::ISODate);
    }

    // Save immediately
    Options::self()->setProperty(key.toLatin1(), value);

    m_Settings[key] = value;
    m_GlobalSettings[key] = value;

    emit settingsUpdated(getAllSettings());
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
QVariantMap Scheduler::getAllSettings() const
{
    QVariantMap settings;

    // All Combo Boxes
    for (auto &oneWidget : findChildren<QComboBox*>())
        settings.insert(oneWidget->objectName(), oneWidget->currentText());

    // All Double Spin Boxes
    for (auto &oneWidget : findChildren<QDoubleSpinBox*>())
        settings.insert(oneWidget->objectName(), oneWidget->value());

    // All Spin Boxes
    for (auto &oneWidget : findChildren<QSpinBox*>())
        settings.insert(oneWidget->objectName(), oneWidget->value());

    // All Checkboxes
    for (auto &oneWidget : findChildren<QCheckBox*>())
        settings.insert(oneWidget->objectName(), oneWidget->isChecked());

    // All Line Edits
    for (auto &oneWidget : findChildren<QLineEdit*>())
    {
        // Many other widget types (e.g. spinboxes) apparently have QLineEdit inside them so we want to skip those
        if (!oneWidget->objectName().startsWith("qt_"))
            settings.insert(oneWidget->objectName(), oneWidget->text());
    }

    // All Radio Buttons
    for (auto &oneWidget : findChildren<QRadioButton*>())
        settings.insert(oneWidget->objectName(), oneWidget->isChecked());

    // All QDateTime
    for (auto &oneWidget : findChildren<QDateTimeEdit*>())
    {
        settings.insert(oneWidget->objectName(), oneWidget->dateTime().toString(Qt::ISODate));
    }

    return settings;
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
void Scheduler::setAllSettings(const QVariantMap &settings)
{
    // Disconnect settings that we don't end up calling syncSettings while
    // performing the changes.
    disconnectSettings();

    for (auto &name : settings.keys())
    {
        // Combo
        auto comboBox = findChild<QComboBox*>(name);
        if (comboBox)
        {
            syncControl(settings, name, comboBox);
            continue;
        }

        // Double spinbox
        auto doubleSpinBox = findChild<QDoubleSpinBox*>(name);
        if (doubleSpinBox)
        {
            syncControl(settings, name, doubleSpinBox);
            continue;
        }

        // spinbox
        auto spinBox = findChild<QSpinBox*>(name);
        if (spinBox)
        {
            syncControl(settings, name, spinBox);
            continue;
        }

        // checkbox
        auto checkbox = findChild<QCheckBox*>(name);
        if (checkbox)
        {
            syncControl(settings, name, checkbox);
            continue;
        }

        // Line Edits
        auto lineedit = findChild<QLineEdit*>(name);
        if (lineedit)
        {
            syncControl(settings, name, lineedit);

            if (name == "sequenceEdit")
                setSequence(lineedit->text());
            else if (name == "fitsEdit")
                processFITSSelection(QUrl::fromLocalFile(lineedit->text()));
            else if (name == "schedulerStartupScript")
                moduleState()->setStartupScriptURL(QUrl::fromUserInput(lineedit->text()));
            else if (name == "schedulerShutdownScript")
                moduleState()->setShutdownScriptURL(QUrl::fromUserInput(lineedit->text()));

            continue;
        }

        // Radio button
        auto radioButton = findChild<QRadioButton*>(name);
        if (radioButton)
        {
            syncControl(settings, name, radioButton);
            continue;
        }

        auto datetimeedit = findChild<QDateTimeEdit*>(name);
        if (datetimeedit)
        {
            syncControl(settings, name, datetimeedit);
            continue;
        }
    }

    m_Settings = settings;

    // Restablish connections
    connectSettings();
}

///////////////////////////////////////////////////////////////////////////////////////////
///
///////////////////////////////////////////////////////////////////////////////////////////
bool Scheduler::syncControl(const QVariantMap &settings, const QString &key, QWidget * widget)
{
    QSpinBox *pSB = nullptr;
    QDoubleSpinBox *pDSB = nullptr;
    QCheckBox *pCB = nullptr;
    QComboBox *pComboBox = nullptr;
    QLineEdit *pLineEdit = nullptr;
    QRadioButton *pRadioButton = nullptr;
    QDateTimeEdit *pDateTimeEdit = nullptr;
    bool ok = true;

    if ((pSB = qobject_cast<QSpinBox *>(widget)))
    {
        const int value = settings[key].toInt(&ok);
        if (ok)
        {
            pSB->setValue(value);
            return true;
        }
    }
    else if ((pDSB = qobject_cast<QDoubleSpinBox *>(widget)))
    {
        const double value = settings[key].toDouble(&ok);
        if (ok)
        {
            pDSB->setValue(value);
            return true;
        }
    }
    else if ((pCB = qobject_cast<QCheckBox *>(widget)))
    {
        const bool value = settings[key].toBool();
        pCB->setChecked(value);
        return true;
    }
    // ONLY FOR STRINGS, not INDEX
    else if ((pComboBox = qobject_cast<QComboBox *>(widget)))
    {
        const QString value = settings[key].toString();
        pComboBox->setCurrentText(value);
        return true;
    }
    else if ((pLineEdit = qobject_cast<QLineEdit *>(widget)))
    {
        const auto value = settings[key].toString();
        pLineEdit->setText(value);
        return true;
    }
    else if ((pRadioButton = qobject_cast<QRadioButton *>(widget)))
    {
        const bool value = settings[key].toBool();
        pRadioButton->setChecked(value);
        return true;
    }
    else if ((pDateTimeEdit = qobject_cast<QDateTimeEdit *>(widget)))
    {
        const auto value = QDateTime::fromString(settings[key].toString(), Qt::ISODate);
        pDateTimeEdit->setDateTime(value);
        return true;
    }

    return false;
};

void Scheduler::connectSettings()
{
    // All Combo Boxes
    for (auto &oneWidget : findChildren<QComboBox*>())
        connect(oneWidget, QOverload<int>::of(&QComboBox::activated), this, &Ekos::Scheduler::syncSettings);

    // All Double Spin Boxes
    for (auto &oneWidget : findChildren<QDoubleSpinBox*>())
        connect(oneWidget, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &Ekos::Scheduler::syncSettings);

    // All Spin Boxes
    for (auto &oneWidget : findChildren<QSpinBox*>())
        connect(oneWidget, QOverload<int>::of(&QSpinBox::valueChanged), this, &Ekos::Scheduler::syncSettings);

    // All Checkboxes
    for (auto &oneWidget : findChildren<QCheckBox*>())
        connect(oneWidget, &QCheckBox::toggled, this, &Ekos::Scheduler::syncSettings);

    // All Radio Butgtons
    for (auto &oneWidget : findChildren<QRadioButton*>())
        connect(oneWidget, &QRadioButton::toggled, this, &Ekos::Scheduler::syncSettings);

    // All QLineEdits
    for (auto &oneWidget : findChildren<QLineEdit*>())
    {
        // Many other widget types (e.g. spinboxes) apparently have QLineEdit inside them so we want to skip those
        if (!oneWidget->objectName().startsWith("qt_"))
            connect(oneWidget, &QLineEdit::textChanged, this, &Ekos::Scheduler::syncSettings);
    }

    // All QDateTimeEdit
    for (auto &oneWidget : findChildren<QDateTimeEdit*>())
        connect(oneWidget, &QDateTimeEdit::dateTimeChanged, this, &Ekos::Scheduler::syncSettings);
}

void Scheduler::disconnectSettings()
{
    // All Combo Boxes
    for (auto &oneWidget : findChildren<QComboBox*>())
        disconnect(oneWidget, QOverload<int>::of(&QComboBox::activated), this, &Ekos::Scheduler::syncSettings);

    // All Double Spin Boxes
    for (auto &oneWidget : findChildren<QDoubleSpinBox*>())
        disconnect(oneWidget, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &Ekos::Scheduler::syncSettings);

    // All Spin Boxes
    for (auto &oneWidget : findChildren<QSpinBox*>())
        disconnect(oneWidget, QOverload<int>::of(&QSpinBox::valueChanged), this, &Ekos::Scheduler::syncSettings);

    // All Checkboxes
    for (auto &oneWidget : findChildren<QCheckBox*>())
        disconnect(oneWidget, &QCheckBox::toggled, this, &Ekos::Scheduler::syncSettings);

    // All Radio Butgtons
    for (auto &oneWidget : findChildren<QRadioButton*>())
        disconnect(oneWidget, &QRadioButton::toggled, this, &Ekos::Scheduler::syncSettings);

    // All QLineEdits
    for (auto &oneWidget : findChildren<QLineEdit*>())
        disconnect(oneWidget, &QLineEdit::editingFinished, this, &Ekos::Scheduler::syncSettings);

    // All QDateTimeEdit
    for (auto &oneWidget : findChildren<QDateTimeEdit*>())
        disconnect(oneWidget, &QDateTimeEdit::editingFinished, this, &Ekos::Scheduler::syncSettings);
}

}
