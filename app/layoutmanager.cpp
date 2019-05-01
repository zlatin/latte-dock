/*
*  Copyright 2017  Smith AR <audoban@openmailbox.org>
*                  Michail Vourlakos <mvourlakos@gmail.com>
*
*  This file is part of Latte-Dock
*
*  Latte-Dock is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License as
*  published by the Free Software Foundation; either version 2 of
*  the License, or (at your option) any later version.
*
*  Latte-Dock is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "layoutmanager.h"

// local
#include "importer.h"
#include "infoview.h"
#include "launcherssignals.h"
#include "screenpool.h"
#include "layout/abstractlayout.h"
#include "layout/activelayout.h"
#include "layout/genericlayout.h"
#include "layout/sharedlayout.h"
#include "settings/settingsdialog.h"
#include "settings/universalsettings.h"
#include "view/view.h"

// Qt
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QQmlProperty>
#include <QtDBus>

// KDE
#include <KActivities/Consumer>
#include <KActivities/Controller>
#include <KLocalizedString>
#include <KNotification>

namespace Latte {

const int MultipleLayoutsPresetId = 10;

LayoutManager::LayoutManager(QObject *parent)
    : QObject(parent),
      m_importer(new Importer(this)),
      m_launchersSignals(new LaunchersSignals(this)),
      m_activitiesController(new KActivities::Controller(this))
{
    m_corona = qobject_cast<Latte::Corona *>(parent);

    if (m_corona) {
        connect(m_corona->universalSettings(), &UniversalSettings::currentLayoutNameChanged, this, &LayoutManager::currentLayoutNameChanged);
        connect(m_corona->universalSettings(), &UniversalSettings::showInfoWindowChanged, this, &LayoutManager::showInfoWindowChanged);

        m_dynamicSwitchTimer.setSingleShot(true);
        showInfoWindowChanged();
        connect(&m_dynamicSwitchTimer, &QTimer::timeout, this, &LayoutManager::confirmDynamicSwitch);
    }
}

LayoutManager::~LayoutManager()
{
    m_importer->deleteLater();
    m_launchersSignals->deleteLater();

    unload();

    m_activitiesController->deleteLater();
}

void LayoutManager::load()
{
    int configVer = m_corona->universalSettings()->version();
    qDebug() << "Universal Settings version : " << configVer;

    if (configVer < 2 && QFile(QDir::homePath() + "/.config/lattedockrc").exists()) {
        qDebug() << "Latte must update its configuration...";
        m_importer->updateOldConfiguration();
        importPresets(false);
    } else if (!QFile(QDir::homePath() + "/.config/lattedockrc").exists()) {
        //startup create what is necessary....
        QDir layoutDir(QDir::homePath() + "/.config/latte");

        if (!layoutDir.exists()) {
            QDir(QDir::homePath() + "/.config").mkdir("latte");
        }

        newLayout(i18n("My Layout"));
        importPresets(false);
        m_corona->universalSettings()->setCurrentLayoutName(i18n("My Layout"));
        m_corona->universalSettings()->setVersion(2);
    }

    //! Check if the multiple-layouts hidden file is present, add it if it isnt
    if (!QFile(QDir::homePath() + "/.config/latte/" + Layout::AbstractLayout::MultipleLayoutsName + ".layout.latte").exists()) {
        importPreset(MultipleLayoutsPresetId, false);
    }

    qDebug() << "Latte is loading  its layouts...";

    connect(m_corona->m_activityConsumer, &KActivities::Consumer::currentActivityChanged,
            this, &LayoutManager::currentActivityChanged);

    connect(m_corona->m_activityConsumer, &KActivities::Consumer::runningActivitiesChanged,
            this, [&]() {
        if (memoryUsage() == Types::MultipleLayouts) {
            syncMultipleLayoutsToActivities();
        }
    });

    loadLayouts();
}

void LayoutManager::unload()
{
    bool multipleMode{activeLayout(Layout::AbstractLayout::MultipleLayoutsName)};

    //! Unload all ActiveLayouts
    while (!m_activeLayouts.isEmpty()) {
        ActiveLayout *layout = m_activeLayouts.at(0);
        m_activeLayouts.removeFirst();

        if (layout->isOriginalLayout() && multipleMode) {
            layout->syncToLayoutFile(true);
        }

        layout->unloadContainments();
        layout->unloadLatteViews();

        if (layout->isOriginalLayout() && multipleMode) {
            clearUnloadedContainmentsFromLinkedFile(layout->unloadedContainmentsIds(), true);
        }

        delete layout;
    }

    //! Unload all SharedLayouts
    while (!m_sharedLayouts.isEmpty()) {
        SharedLayout *layout = m_sharedLayouts.at(0);
        m_sharedLayouts.removeFirst();

        if (multipleMode) {
            layout->syncToLayoutFile(true);
        }

        layout->unloadContainments();
        layout->unloadLatteViews();

        if (multipleMode) {
            clearUnloadedContainmentsFromLinkedFile(layout->unloadedContainmentsIds(), true);
        }

        delete layout;
    }

    //! Remove no-needed temp files
    QString temp1File = QDir::homePath() + "/.config/lattedock.copy1.bak";
    QString temp2File = QDir::homePath() + "/.config/lattedock.copy2.bak";

    QFile file1(temp1File);
    QFile file2(temp2File);

    if (file1.exists())
        file1.remove();

    if (file2.exists())
        file2.remove();
}

Latte::Corona *LayoutManager::corona()
{
    return m_corona;
}

Importer *LayoutManager::importer()
{
    return m_importer;
}

LaunchersSignals *LayoutManager::launchersSignals()
{
    return m_launchersSignals;
}

QString LayoutManager::currentLayoutName() const
{
    if (memoryUsage() == Types::SingleLayout) {
        return m_corona->universalSettings()->currentLayoutName();
    } else if (memoryUsage() == Types::MultipleLayouts) {
        return m_currentLayoutNameInMultiEnvironment;
    }

    return QString();
}

QString LayoutManager::defaultLayoutName() const
{
    QByteArray presetNameOrig = QString("preset" + QString::number(1)).toUtf8();
    QString presetPath = m_corona->kPackage().filePath(presetNameOrig);
    QString presetName = ActiveLayout::layoutName(presetPath);
    QByteArray presetNameChars = presetName.toUtf8();
    presetName = i18n(presetNameChars);

    return presetName;
}

bool LayoutManager::layoutExists(QString layoutName) const
{
    return m_layouts.contains(layoutName);
}

QStringList LayoutManager::layouts() const
{
    return m_layouts;
}

QStringList LayoutManager::menuLayouts() const
{
    QStringList fixedMenuLayouts = m_menuLayouts;

    //! in case the current layout isnt checked to be shown in the menus
    //! we must add it on top
    if (!fixedMenuLayouts.contains(currentLayoutName()) && memoryUsage() == Types::SingleLayout) {
        fixedMenuLayouts.prepend(currentLayoutName());
    } else if (memoryUsage() == Types::MultipleLayouts) {
        for (const auto layout : m_activeLayouts) {
            if (layout->isOriginalLayout() && !fixedMenuLayouts.contains(layout->name())) {
                fixedMenuLayouts.prepend(layout->name());
            }
        }
    }

    return fixedMenuLayouts;
}

void LayoutManager::setMenuLayouts(QStringList layouts)
{
    if (m_menuLayouts == layouts) {
        return;
    }

    m_menuLayouts = layouts;
    emit menuLayoutsChanged();
}

QStringList LayoutManager::activities()
{
    return m_corona->m_activityConsumer->activities();
}

QStringList LayoutManager::runningActivities()
{
    return m_corona->m_activityConsumer->runningActivities();
}

QStringList LayoutManager::orphanedActivities()
{
    QStringList orphans;

    for (const auto &activity : activities()) {
        if (m_assignedLayouts[activity].isEmpty()) {
            orphans.append(activity);
        }
    }

    return orphans;
}

QStringList LayoutManager::presetsPaths() const
{
    return m_presetsPaths;
}

QString LayoutManager::layoutPath(QString layoutName)
{
    QString path = QDir::homePath() + "/.config/latte/" + layoutName + ".layout.latte";

    if (!QFile(path).exists()) {
        path = "";
    }

    return path;
}

Types::LayoutsMemoryUsage LayoutManager::memoryUsage() const
{
    return m_corona->universalSettings()->layoutsMemoryUsage();
}

int LayoutManager::layoutsMemoryUsage()
{
    return (int)m_corona->universalSettings()->layoutsMemoryUsage();
}

void LayoutManager::setMemoryUsage(Types::LayoutsMemoryUsage memoryUsage)
{
    m_corona->universalSettings()->setLayoutsMemoryUsage(memoryUsage);
}

bool LayoutManager::latteViewExists(Latte::View *view) const
{
    for (const auto layout : m_activeLayouts) {
        for (const auto &v : layout->latteViews()) {
            if (v == view) {
                return true;
            }
        }
    }

    return false;
}

QStringList LayoutManager::activeLayoutsNames()
{
    QStringList names;

    if (memoryUsage() == Types::SingleLayout) {
        names << currentLayoutName();
    } else {
        for (int i = 0; i < m_activeLayouts.size(); ++i) {
            ActiveLayout *layout = m_activeLayouts.at(i);

            if (layout->isOriginalLayout()) {
                names << layout->name();
            }
        }
    }

    return names;

}

Layout::GenericLayout *LayoutManager::layout(QString id) const
{
    Layout::GenericLayout *l = activeLayout(id);

    if (!l) {
        l = sharedLayout(id);
    }

    return l;
}


ActiveLayout *LayoutManager::activeLayout(QString id) const
{
    for (int i = 0; i < m_activeLayouts.size(); ++i) {
        ActiveLayout *layout = m_activeLayouts.at(i);

        if (layout->name() == id) {

            return layout;
        }
    }

    return nullptr;
}

int LayoutManager::activeLayoutPos(QString id) const
{
    for (int i = 0; i < m_activeLayouts.size(); ++i) {
        ActiveLayout *layout = m_activeLayouts.at(i);

        if (layout->name() == id) {

            return i;
        }
    }

    return -1;
}

SharedLayout *LayoutManager::sharedLayout(QString id) const
{
    for (int i = 0; i < m_sharedLayouts.size(); ++i) {
        SharedLayout *layout = m_sharedLayouts.at(i);

        if (layout->name() == id) {
            return layout;
        }
    }

    return nullptr;
}

bool LayoutManager::assignActiveToSharedLayout(ActiveLayout *active, QString id)
{
    if (memoryUsage() == Types::SingleLayout) {
        return false;
    }

    for (int i = 0; i < m_sharedLayouts.size(); ++i) {
        SharedLayout *layout = m_sharedLayouts.at(i);

        if (layout->name() == id) {
            layout->addActiveLayout(active);
         //   syncLatteViewsToScreens();
            return true;
        }
    }

    //! If SharedLayout was not found, we must create it
    SharedLayout *top = new SharedLayout(active, this, Importer::layoutFilePath(id));
    m_sharedLayouts.append(top);
    top->importToCorona();
 //   syncLatteViewsToScreens();

    return true;
}

ActiveLayout *LayoutManager::currentLayout() const
{
    if (memoryUsage() == Types::SingleLayout) {
        return m_activeLayouts.at(0);
    } else {
        for (auto layout : m_activeLayouts) {
            if (layout->activities().contains(m_corona->m_activityConsumer->currentActivity())) {
                return layout;
            }
        }

        for (auto layout : m_activeLayouts) {
            if ((layout->name() != Layout::AbstractLayout::MultipleLayoutsName) && (layout->activities().isEmpty())) {
                return layout;
            }
        }
    }

    return nullptr;
}

void LayoutManager::updateCurrentLayoutNameInMultiEnvironment()
{
    for (const auto layout : m_activeLayouts) {
        if (layout->isOriginalLayout() && layout->activities().contains(m_corona->activitiesConsumer()->currentActivity())) {
            m_currentLayoutNameInMultiEnvironment = layout->name();
            emit currentLayoutNameChanged();
            return;
        }
    }

    for (const auto layout : m_activeLayouts) {
        if (layout->isOriginalLayout() && layout->activities().isEmpty()) {
            m_currentLayoutNameInMultiEnvironment = layout->name();
            emit currentLayoutNameChanged();
            return;
        }
    }
}

void LayoutManager::currentActivityChanged(const QString &id)
{
    if (memoryUsage() == Types::SingleLayout) {
        qDebug() << "activity changed :: " << id;

        m_shouldSwitchToLayout = shouldSwitchToLayout(id);

        m_dynamicSwitchTimer.start();
    } else if (memoryUsage() == Types::MultipleLayouts) {
        updateCurrentLayoutNameInMultiEnvironment();
    }
}

void LayoutManager::showInfoWindowChanged()
{
    if (m_corona->universalSettings()->showInfoWindow()) {
        m_dynamicSwitchTimer.setInterval(1800);
    } else {
        m_dynamicSwitchTimer.setInterval(2300);
    }
}

QString LayoutManager::shouldSwitchToLayout(QString activityId)
{
    if (m_assignedLayouts.contains(activityId) && m_assignedLayouts[activityId] != currentLayoutName()) {
        return m_assignedLayouts[activityId];
    } else if (!m_assignedLayouts.contains(activityId) && !m_corona->universalSettings()->lastNonAssignedLayoutName().isEmpty()
               && m_corona->universalSettings()->lastNonAssignedLayoutName() != currentLayoutName()) {
        return m_corona->universalSettings()->lastNonAssignedLayoutName();
    }

    return QString();
}

void LayoutManager::confirmDynamicSwitch()
{
    QString tempShouldSwitch = shouldSwitchToLayout(m_corona->m_activityConsumer->currentActivity());

    if (tempShouldSwitch.isEmpty()) {
        return;
    }

    if (m_shouldSwitchToLayout == tempShouldSwitch && m_shouldSwitchToLayout != currentLayoutName()) {
        qDebug() << "dynamic switch to layout :: " << m_shouldSwitchToLayout;

        emit currentLayoutIsSwitching(currentLayoutName());

        if (m_corona->universalSettings()->showInfoWindow()) {
            showInfoWindow(i18n("Switching to layout <b>%0</b> ...").arg(m_shouldSwitchToLayout), 4000);
        }

        QTimer::singleShot(500, [this, tempShouldSwitch]() {
            switchToLayout(tempShouldSwitch);
        });
    } else {
        m_shouldSwitchToLayout = tempShouldSwitch;
        m_dynamicSwitchTimer.start();
    }
}

void LayoutManager::loadLayouts()
{
    m_layouts.clear();
    m_menuLayouts.clear();
    m_presetsPaths.clear();
    m_assignedLayouts.clear();

    QDir layoutDir(QDir::homePath() + "/.config/latte");
    QStringList filter;
    filter.append(QString("*.layout.latte"));
    QStringList files = layoutDir.entryList(filter, QDir::Files | QDir::NoSymLinks);

    for (const auto &layout : files) {
        ActiveLayout layoutSets(this, layoutDir.absolutePath() + "/" + layout);

        QStringList validActivityIds = validActivities(layoutSets.activities());
        layoutSets.setActivities(validActivityIds);

        for (const auto &activity : validActivityIds) {
            m_assignedLayouts[activity] = layoutSets.name();
        }

        m_layouts.append(layoutSets.name());

        if (layoutSets.showInMenu()) {
            m_menuLayouts.append(layoutSets.name());
        }
    }

    m_presetsPaths.append(m_corona->kPackage().filePath("preset1"));
    m_presetsPaths.append(m_corona->kPackage().filePath("preset2"));
    m_presetsPaths.append(m_corona->kPackage().filePath("preset3"));
    m_presetsPaths.append(m_corona->kPackage().filePath("preset4"));

    emit layoutsChanged();
    emit menuLayoutsChanged();
}

void LayoutManager::loadLayoutOnStartup(QString layoutName)
{
    // if (memoryUsage() == Types::MultipleLayouts) {
    QStringList layouts = m_importer->checkRepairMultipleLayoutsLinkedFile();

    //! Latte didn't close correctly, maybe a crash
    if (layouts.size() > 0) {
        QMessageBox *msg = new QMessageBox();
        msg->setAttribute(Qt::WA_DeleteOnClose);
        msg->setIcon(QMessageBox::Warning);
        msg->setWindowTitle(i18n("Multiple Layouts Warning"));
        msg->setText(i18n("Latte did not close properly in the previous session. The following layout(s) <b>[%0]</b> were updated for consistency!!!").arg(layouts.join(",")));
        msg->setStandardButtons(QMessageBox::Ok);

        msg->open();
    }

    //}

    switchToLayout(layoutName);
}

void LayoutManager::loadLatteLayout(QString layoutPath)
{
    qDebug() << " -------------------------------------------------------------------- ";
    qDebug() << " -------------------------------------------------------------------- ";

    if (m_corona->containments().size() > 0) {
        qDebug() << "LOAD LATTE LAYOUT ::: There are still containments present !!!! :: " << m_corona->containments().size();
    }

    if (!layoutPath.isEmpty() && m_corona->containments().size() == 0) {
        cleanupOnStartup(layoutPath);
        qDebug() << "LOADING CORONA LAYOUT:" << layoutPath;
        m_corona->loadLayout(layoutPath);

        //! ~~~ ADDING LATTEVIEWS AND ENFORCE LOADING IF TASKS ARENT PRESENT BASED ON SCREENS ~~~ !//

        //! this is used to record the first dock having tasks in it. It is used
        //! to specify which dock will be loaded on startup if a case that no "dock
        //! with tasks" will be loaded otherwise. Currently the older one dock wins
        /*int firstContainmentWithTasks = -1;

        //! this is used to check if a dock with tasks in it will be loaded on startup
        bool tasksWillBeLoaded =  heuresticForLoadingDockWithTasks(&firstContainmentWithTasks);

        qDebug() << "TASKS WILL BE PRESENT AFTER LOADING ::: " << tasksWillBeLoaded;

        for (const auto containment : m_corona->containments()) {
            //! forceDockLoading is used when a latte configuration based on the
            //! current running screens does not provide a dock containing tasks.
            //! in such case the lowest latte containment containing tasks is loaded
            //! and it forcefully becomes primary dock
            if (!tasksWillBeLoaded && firstContainmentWithTasks == containment->id()) {
                tasksWillBeLoaded = true; //this protects by loading more than one dock at startup
                addDock(containment, true);
            } else {
                addDock(containment);
            }
        }*/
    }
}

void LayoutManager::cleanupOnStartup(QString path)
{
    KSharedConfigPtr filePtr = KSharedConfig::openConfig(path);

    KConfigGroup actionGroups = KConfigGroup(filePtr, "ActionPlugins");

    QStringList deprecatedActionGroup;

    for (const auto &actId : actionGroups.groupList()) {
        QString pluginId = actionGroups.group(actId).readEntry("RightButton;NoModifier", "");

        if (pluginId == "org.kde.contextmenu") {
            deprecatedActionGroup << actId;
        }
    }

    for (const auto &pId : deprecatedActionGroup) {
        qDebug() << "!!!!!!!!!!!!!!!!  !!!!!!!!!!!! !!!!!!! REMOVING :::: " << pId;
        actionGroups.group(pId).deleteGroup();
    }

    KConfigGroup containmentGroups = KConfigGroup(filePtr, "Containments");

    QStringList removeContaimentsList;

    for (const auto &cId : containmentGroups.groupList()) {
        QString pluginId = containmentGroups.group(cId).readEntry("plugin", "");

        if (pluginId == "org.kde.desktopcontainment") { //!must remove ghost containments first
            removeContaimentsList << cId;
        }
    }

    for (const auto &cId : removeContaimentsList) {
        containmentGroups.group(cId).deleteGroup();
    }


    actionGroups.sync();
    containmentGroups.sync();
}


void LayoutManager::showAboutDialog()
{
    m_corona->aboutApplication();
}

void LayoutManager::importLatteLayout(QString layoutPath)
{
    //! This might not be needed as it is Layout responsibility
}

void LayoutManager::hideAllViews()
{
    for (const auto layout : m_activeLayouts) {
        if (layout->isOriginalLayout()) {
            emit currentLayoutIsSwitching(layout->name());
        }
    }
}

void LayoutManager::addLayout(ActiveLayout *layout)
{
    if (!m_activeLayouts.contains(layout)) {
        m_activeLayouts.append(layout);
        layout->initToCorona(m_corona);
    }
}

bool LayoutManager::switchToLayout(QString layoutName, int previousMemoryUsage)
{
    if (m_activeLayouts.size() > 0 && currentLayoutName() == layoutName && previousMemoryUsage == -1) {
        return false;
    }

    //! First Check If that Layout is already present and in that case
    //! we can just switch to the proper Activity
    if (memoryUsage() == Types::MultipleLayouts && previousMemoryUsage == -1) {
        ActiveLayout *layout = activeLayout(layoutName);

        if (layout) {

            QStringList appliedActivities = layout->appliedActivities();
            QString nextActivity = !layout->lastUsedActivity().isEmpty() ? layout->lastUsedActivity() : appliedActivities[0];

            //! it means we are at a foreign activity
            if (!appliedActivities.contains(m_corona->activitiesConsumer()->currentActivity())) {
                m_activitiesController->setCurrentActivity(nextActivity);
                return true;
            }
        }
    }

    //! When going from memory usage to different memory usage we first
    //! send the layouts that will be changed. This signal creates the
    //! nice animation that hides these docks/panels
    if (previousMemoryUsage != -1) {
        for (const auto layout : m_activeLayouts) {
            if (layout->isOriginalLayout()) {
                emit currentLayoutIsSwitching(layout->name());
            }
        }

        for (const auto layout : m_sharedLayouts) {
            emit currentLayoutIsSwitching(layout->name());
        }
    }

    QString lPath = layoutPath(layoutName);

    if (lPath.isEmpty() && layoutName == i18n("Alternative")) {
        lPath = newLayout(i18n("Alternative"), i18n("Default"));
    }

    if (!lPath.isEmpty()) {
        if (memoryUsage() == Types::SingleLayout) {
          //  emit currentLayoutIsSwitching(currentLayoutName());
        } else if (memoryUsage() == Types::MultipleLayouts && layoutName != Layout::AbstractLayout::MultipleLayoutsName) {
            ActiveLayout toLayout(this, lPath);

            QStringList toActivities = toLayout.activities();

            ActiveLayout *activeForOrphans{nullptr};

            for (const auto fromLayout : m_activeLayouts) {
                if (fromLayout->isOriginalLayout() && fromLayout->activities().isEmpty()) {
                    activeForOrphans = fromLayout;
                    break;
                }
            }

            if (toActivities.isEmpty() &&  activeForOrphans && (toLayout.name() != activeForOrphans->name())) {
                emit currentLayoutIsSwitching(activeForOrphans->name());
            }
        }

        //! this code must be called asynchronously because it is called
        //! also from qml (Tasks plasmoid). This change fixes a very important
        //! crash when switching sessions through the Tasks plasmoid Context menu
        //! Latte was unstable and was crashing very often during changing
        //! sessions.
        QTimer::singleShot(350, [this, layoutName, lPath, previousMemoryUsage]() {
            qDebug() << layoutName << " - " << lPath;
            QString fixedLPath = lPath;
            QString fixedLayoutName = layoutName;

            bool initializingMultipleLayouts{false};

            if (memoryUsage() == Types::MultipleLayouts && !activeLayout(Layout::AbstractLayout::MultipleLayoutsName)) {
                initializingMultipleLayouts = true;
            }

            if (memoryUsage() == Types::SingleLayout || initializingMultipleLayouts || previousMemoryUsage == Types::MultipleLayouts) {
                unload();

                if (initializingMultipleLayouts) {
                    fixedLayoutName = QString(Layout::AbstractLayout::MultipleLayoutsName);
                    fixedLPath = layoutPath(fixedLayoutName);
                }

                ActiveLayout *newLayout = new ActiveLayout(this, fixedLPath, fixedLayoutName);
                addLayout(newLayout);
                loadLatteLayout(fixedLPath);

                emit activeLayoutsChanged();
            }

            if (memoryUsage() == Types::MultipleLayouts) {
                if (!initializingMultipleLayouts && !activeLayout(layoutName)) {
                    //! When we are in Multiple Layouts Environment and the user activates
                    //! a Layout that is assigned to specific activities but this
                    //! layout isnt loaded (this means neither of its activities are running)
                    //! is such case we just activate these Activities
                    ActiveLayout layout(this, Importer::layoutFilePath(layoutName));

                    int i = 0;
                    bool lastUsedActivityFound{false};
                    QString lastUsedActivity = layout.lastUsedActivity();

                    bool orphanedLayout = !layoutIsAssigned(layoutName);

                    QStringList assignedActivities = orphanedLayout ? orphanedActivities() : layout.activities();

                    if (!orphanedLayout) {
                        for (const auto &assignedActivity : assignedActivities) {
                            //! Starting the activities must be done asynchronous because otherwise
                            //! the activity manager cant close multiple activities
                            QTimer::singleShot(i * 1000, [this, assignedActivity, lastUsedActivity]() {
                                m_activitiesController->startActivity(assignedActivity);

                                if (lastUsedActivity == assignedActivity) {
                                    m_activitiesController->setCurrentActivity(lastUsedActivity);
                                }
                            });

                            if (lastUsedActivity == assignedActivity) {
                                lastUsedActivityFound = true;
                            }

                            i = i + 1;
                        }
                    } else {
                        //! orphaned layout
                        for (const auto &assignedActivity : assignedActivities) {
                            if (lastUsedActivity == assignedActivity) {
                                lastUsedActivityFound = true;
                            }
                        }

                        if ((!lastUsedActivityFound && assignedActivities.count() == 0)
                                || !assignedActivities.contains(m_corona->m_activityConsumer->currentActivity())) {

                            //! Starting the activities must be done asynchronous because otherwise
                            //! the activity manager cant close multiple activities
                            QTimer::singleShot(1000, [this, lastUsedActivity, lastUsedActivityFound]() {
                                m_activitiesController->startActivity(lastUsedActivity);
                                m_activitiesController->setCurrentActivity(lastUsedActivity);
                            });
                        }
                    }

                    if (orphanedLayout) {
                        syncMultipleLayoutsToActivities(layoutName);
                    } else if (!orphanedLayout && !lastUsedActivityFound) {
                        m_activitiesController->setCurrentActivity(layout.activities()[0]);
                    }
                } else {
                    syncMultipleLayoutsToActivities(layoutName);
                }
            }

            m_corona->universalSettings()->setCurrentLayoutName(layoutName);

            if (!layoutIsAssigned(layoutName)) {
                m_corona->universalSettings()->setLastNonAssignedLayoutName(layoutName);
            }
        });
    } else {
        qDebug() << "Layout : " << layoutName << " was not found...";
    }

    return true;
}

void LayoutManager::syncMultipleLayoutsToActivities(QString layoutForOrphans)
{
    qDebug() << "   ----  --------- ------    syncMultipleLayoutsToActivities       -------   ";
    qDebug() << "   ----  --------- ------    -------------------------------       -------   ";

    QStringList layoutsToUnload;
    QStringList layoutsToLoad;
    layoutsToLoad << Layout::AbstractLayout::MultipleLayoutsName;

    bool allRunningActivitiesWillBeReserved{true};

    if (layoutForOrphans.isEmpty() || m_assignedLayouts.values().contains(layoutForOrphans)) {
        layoutForOrphans = m_corona->universalSettings()->lastNonAssignedLayoutName();
    }

    for (const auto &activity : runningActivities()) {
        if (!m_assignedLayouts[activity].isEmpty()) {
            if (!layoutsToLoad.contains(m_assignedLayouts[activity])) {
                layoutsToLoad.append(m_assignedLayouts[activity]);
            }
        } else {
            allRunningActivitiesWillBeReserved = false;
        }
    }

    for (const auto layout : m_activeLayouts) {
        QString tempLayoutName;

        if (!layoutsToLoad.contains(layout->name()) && layout->name() != layoutForOrphans) {
            tempLayoutName = layout->name();
        } else if (layout->activities().isEmpty() && allRunningActivitiesWillBeReserved) {
            //! in such case the layout for the orphaned must be unloaded
            tempLayoutName = layout->name();
        }

        if (!tempLayoutName.isEmpty() && !layoutsToUnload.contains(tempLayoutName)) {
            layoutsToUnload << tempLayoutName;
        }
    }

    //! Unload no needed Layouts
    for (const auto &layoutName : layoutsToUnload) {
        if (layoutName != Layout::AbstractLayout::MultipleLayoutsName) {
            ActiveLayout *layout = activeLayout(layoutName);
            int posLayout = activeLayoutPos(layoutName);

            if (posLayout >= 0) {
                qDebug() << "REMOVING LAYOUT ::::: " << layoutName;
                m_activeLayouts.removeAt(posLayout);

                if (layout->isOriginalLayout()) {
                    layout->syncToLayoutFile(true);
                }

                layout->unloadContainments();
                layout->unloadLatteViews();
                clearUnloadedContainmentsFromLinkedFile(layout->unloadedContainmentsIds());
                delete layout;
            }
        }
    }

    //! Add Layout for orphan activities
    if (!allRunningActivitiesWillBeReserved) {
        if (!activeLayout(layoutForOrphans)) {
            ActiveLayout *newLayout = new ActiveLayout(this, layoutPath(layoutForOrphans), layoutForOrphans);

            if (newLayout) {
                qDebug() << "ACTIVATING ORPHANED LAYOUT ::::: " << layoutForOrphans;
                addLayout(newLayout);
                newLayout->importToCorona();
            }
        }
    }

    //! Add needed Layouts based on Activities
    for (const auto &layoutName : layoutsToLoad) {
        if (!activeLayout(layoutName)) {
            ActiveLayout *newLayout = new ActiveLayout(this, QString(layoutPath(layoutName)), layoutName);

            if (newLayout) {
                qDebug() << "ACTIVATING LAYOUT ::::: " << layoutName;
                addLayout(newLayout);
                newLayout->importToCorona();

                if (newLayout->isOriginalLayout() && m_corona->universalSettings()->showInfoWindow()) {
                    showInfoWindow(i18n("Activating layout: <b>%0</b> ...").arg(newLayout->name()), 5000, newLayout->appliedActivities());
                }
            }
        }
    }

    updateCurrentLayoutNameInMultiEnvironment();
    emit activeLayoutsChanged();
}

void LayoutManager::pauseLayout(QString layoutName)
{
    if (memoryUsage() == Types::MultipleLayouts) {
        ActiveLayout *layout = activeLayout(layoutName);

        if (layout && !layout->activities().isEmpty()) {
            int i = 0;

            for (const auto &activityId : layout->activities()) {
                //! Stopping the activities must be done asynchronous because otherwise
                //! the activity manager cant close multiple activities
                QTimer::singleShot(i * 1000, [this, activityId]() {
                    m_activitiesController->stopActivity(activityId);
                });

                i = i + 1;
            }
        }
    }
}

void LayoutManager::syncActiveLayoutsToOriginalFiles()
{
    if (memoryUsage() == Types::MultipleLayouts) {
        for (const auto layout : m_activeLayouts) {
            if (layout->isOriginalLayout()) {
                layout->syncToLayoutFile();
            }
        }
    }
}

void LayoutManager::clearUnloadedContainmentsFromLinkedFile(QStringList containmentsIds, bool bypassChecks)
{
    if (!m_corona || (memoryUsage() == Types::SingleLayout && !bypassChecks)) {
        return;
    }

    auto containments = m_corona->config()->group("Containments");

    for (const auto &conId : containmentsIds) {
        qDebug() << "unloads ::: " << conId;
        KConfigGroup containment = containments.group(conId);
        containment.deleteGroup();
    }

    containments.sync();
}


void LayoutManager::syncLatteViewsToScreens()
{
    for (const auto layout : m_sharedLayouts) {
        layout->syncLatteViewsToScreens();
    }

    for (const auto layout : m_activeLayouts) {
        layout->syncLatteViewsToScreens();
    }
}

QString LayoutManager::newLayout(QString layoutName, QString preset)
{
    QDir layoutDir(QDir::homePath() + "/.config/latte");
    QStringList filter;
    filter.append(QString(layoutName + "*.layout.latte"));
    QStringList files = layoutDir.entryList(filter, QDir::Files | QDir::NoSymLinks);

    //! if the newLayout already exists provide a newName that doesn't
    if (files.count() >= 1) {
        int newCounter = files.count() + 1;

        layoutName = layoutName + "-" + QString::number(newCounter);
    }

    QString newLayoutPath = layoutDir.absolutePath() + "/" + layoutName + ".layout.latte";

    qDebug() << "adding layout : " << layoutName << " based on preset:" << preset;

    if (preset == i18n("Default") && !QFile(newLayoutPath).exists()) {
        qDebug() << "adding layout : succeed";
        QFile(m_corona->kPackage().filePath("preset1")).copy(newLayoutPath);
    }

    return newLayoutPath;
}

//! This function figures in the beginning if a view with tasks
//! in it will be loaded taking into account also the screens are present.
bool LayoutManager::heuresticForLoadingViewWithTasks(int *firstContainmentWithTasks)
{
    for (const auto containment : m_corona->containments()) {
        QString plugin = containment->pluginMetaData().pluginId();

        if (plugin == "org.kde.latte.containment") {
            bool onPrimary = containment->config().readEntry("onPrimary", true);
            int lastScreen =  containment->lastScreen();

            qDebug() << "containment values: " << onPrimary << " - " << lastScreen;


            bool containsTasks = false;

            for (const  auto applet : containment->applets()) {
                const auto &provides = KPluginMetaData::readStringList(applet->pluginMetaData().rawData(), QStringLiteral("X-Plasma-Provides"));

                if (provides.contains(QLatin1String("org.kde.plasma.multitasking"))) {
                    containsTasks = true;
                    break;
                }
            }

            if (containsTasks) {
                *firstContainmentWithTasks = containment->id();

                if (onPrimary) {
                    return true;
                } else {
                    if (m_corona->screenPool()->hasId(lastScreen)) {
                        QString connector = m_corona->screenPool()->connector(lastScreen);

                        for (const auto scr : qGuiApp->screens()) {
                            if (scr && scr->name() == connector) {
                                return true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}


void LayoutManager::importDefaultLayout(bool newInstanceIfPresent)
{
    importPreset(1, newInstanceIfPresent);

    if (newInstanceIfPresent) {
        loadLayouts();
    }
}

void LayoutManager::importPresets(bool includeDefault)
{
    int start = 1;

    if (!includeDefault) {
        start = 2;
    }

    for (int i = start; i <= 4; ++i) {
        importPreset(i, false);
    }
}

void LayoutManager::importPreset(int presetNo, bool newInstanceIfPresent)
{
    QDir configDir(QDir::homePath() + "/.config");

    if (!QDir(configDir.absolutePath() + "/latte").exists()) {
        configDir.mkdir("latte");
    }

    QByteArray presetNameOrig = QString("preset" + QString::number(presetNo)).toUtf8();
    QString presetPath = m_corona->kPackage().filePath(presetNameOrig);
    QString presetName = ActiveLayout::layoutName(presetPath);
    QByteArray presetNameChars = presetName.toUtf8();
    presetName = i18n(presetNameChars);

    //! hide the multiple layouts layout file from user
    presetName = (presetNo == MultipleLayoutsPresetId) ? "." + presetName : presetName;

    QString newLayoutFile = "";

    if (newInstanceIfPresent) {
        newLayoutFile = QDir::homePath() + "/.config/latte/" + m_importer->uniqueLayoutName(presetName) + ".layout.latte";
    } else {
        newLayoutFile = QDir::homePath() + "/.config/latte/" + presetName + ".layout.latte";
    }

    if (!QFile(newLayoutFile).exists()) {
        QFile(presetPath).copy(newLayoutFile);
        QFileInfo newFileInfo(newLayoutFile);

        if (newFileInfo.exists() && !newFileInfo.isWritable()) {
            QFile(newLayoutFile).setPermissions(QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        }
    }
}

QStringList LayoutManager::validActivities(QStringList currentList)
{
    QStringList validIds;

    for (const auto &activity : currentList) {
        if (activities().contains(activity)) {
            validIds.append(activity);
        }
    }

    return validIds;
}

bool LayoutManager::layoutIsAssigned(QString layoutName)
{
    QHashIterator<const QString, QString> i(m_assignedLayouts);

    while (i.hasNext()) {
        i.next();

        if (i.value() == layoutName) {
            return true;
        }
    }

    return false;
}

void LayoutManager::showLatteSettingsDialog(int page)
{
    if (!m_latteSettingsDialog) {
        m_latteSettingsDialog = new SettingsDialog(nullptr, m_corona);
    }

    m_latteSettingsDialog->show();

    if (m_latteSettingsDialog->isMinimized()) {
        m_latteSettingsDialog->showNormal();
    }

    Types::LatteConfigPage configPage = static_cast<Types::LatteConfigPage>(page);
    m_latteSettingsDialog->setCurrentPage(configPage);

    m_latteSettingsDialog->activateWindow();
}

void LayoutManager::hideLatteSettingsDialog()
{
    if (m_latteSettingsDialog) {
        m_latteSettingsDialog->deleteLater();
        m_latteSettingsDialog = nullptr;
    }
}

void LayoutManager::showInfoWindow(QString info, int duration, QStringList activities)
{
    for (const auto screen : qGuiApp->screens()) {
        InfoView *infoView = new InfoView(m_corona, info, screen);

        infoView->show();
        infoView->setOnActivities(activities);

        QTimer::singleShot(duration, [this, infoView]() {
            infoView->deleteLater();
        });
    }
}

//! it is used just in order to provide translations for the presets
void LayoutManager::ghostForTranslatedPresets()
{
    QString preset1 = i18n("Default");
    QString preset2 = i18n("Plasma");
    QString preset3 = i18n("Unity");
    QString preset4 = i18n("Extended");
}

}
