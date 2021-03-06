#include "StandAloneMainController.h"

#include "midi/RtMidiDriver.h"
#include "audio/core/PortAudioDriver.h"

#include "audio/vst/VstPlugin.h"
#include "audio/vst/VstHost.h"
#include "audio/vst/PluginFinder.h"
#include "audio/core/PluginDescriptor.h"
#include "NinjamController.h"
#include "MainWindowStandalone.h"

#include <QDialog>
#include <QHostAddress>

#if _WIN32
    #include "windows.h"
#endif

#include <QDataStream>
#include <QFile>
#include <QDirIterator>
#include <QSettings>
#include <QtConcurrent/QtConcurrent>
#include "log/Logging.h"
#include "Configurator.h"

using namespace Controller;

// +++++++++++++++++++++++++

StandalonePluginFinder::StandalonePluginFinder()
{
}

StandalonePluginFinder::~StandalonePluginFinder()
{
}

Audio::PluginDescriptor StandalonePluginFinder::getPluginDescriptor(QFileInfo f)
{
    QString name = Audio::PluginDescriptor::getPluginNameFromPath(f.absoluteFilePath());
    return Audio::PluginDescriptor(name, "VST", f.absoluteFilePath());
}

void StandalonePluginFinder::on_processFinished()
{
    QProcess::ExitStatus exitStatus = scanProcess.exitStatus();
    scanProcess.close();
    bool exitingWithoutError = exitStatus == QProcess::NormalExit;
    emit scanFinished(exitingWithoutError);
    QString lastScanned(lastScannedPlugin);
    lastScannedPlugin.clear();
    if (!exitingWithoutError)
        handleProcessError(lastScanned);
}

void StandalonePluginFinder::on_processError(QProcess::ProcessError error)
{
    qCritical(jtStandalonePluginFinder) << "ERROR:" << error << scanProcess.errorString();
    on_processFinished();
}

void StandalonePluginFinder::handleProcessError(QString lastScannedPlugin)
{
    if (!lastScannedPlugin.isEmpty())
        emit badPluginDetected(lastScannedPlugin);
}

QString StandalonePluginFinder::getVstScannerExecutablePath() const
{
    // try the same jamtaba executable path first
    QString scannerExePath = QApplication::applicationDirPath() + "/VstScanner";// In the deployed version the VstScanner and Jamtaba2 executables are in the same folder.
#ifdef Q_OS_WIN
    scannerExePath += ".exe";
#endif
    if (QFile(scannerExePath).exists())
        return scannerExePath;
    else
        qWarning(jtStandalonePluginFinder) << "Scanner exe not founded in" << scannerExePath;

    // In dev time the executable (Jamtaba2 and VstScanner) are in different folders...
    // We need a more elegant way to solve this at dev time. At moment I'm a very dirst approach to return the executable path in MacOsx, and just a little less dirty solution in windows.
    QString appPath = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MAC
    return
        "/Users/elieser/Desktop/build-Jamtaba-Desktop_Qt_5_5_0_clang_64bit-Debug/VstScanner/VstScanner";
#endif
    QString buildType = QLibraryInfo::isDebugBuild() ? "debug" : "release";
    scannerExePath = appPath + "/../../VstScanner/"+ buildType +"/VstScanner.exe";
    if (QFile(scannerExePath).exists())
        return scannerExePath;
    qCCritical(jtStandalonePluginFinder) << "Vst scanner exeutable not found in" << scannerExePath;
    return "";
}

void StandalonePluginFinder::on_processStandardOutputReady()
{
    QByteArray readedData = scanProcess.readAll();
    QTextStream stream(readedData, QIODevice::ReadOnly);
    while (!stream.atEnd()) {
        QString readedLine = stream.readLine();
        if (!readedLine.isNull() && !readedLine.isEmpty()) {
            bool startScanning = readedLine.startsWith("JT-Scanner-Scanning:");
            bool finishedScanning = readedLine.startsWith("JT-Scanner-Scan-Finished");
            if (startScanning || finishedScanning) {
                QString pluginPath = readedLine.split(": ").at(1);
                if (startScanning) {
                    lastScannedPlugin = pluginPath;// store the plugin path, if the scanner process crash we can add this bad plugin in the black list
                    emit pluginScanStarted(pluginPath);
                } else {
                    QString pluginName = Audio::PluginDescriptor::getPluginNameFromPath(pluginPath);
                    emit pluginScanFinished(pluginName, "VST", pluginPath);
                }
            }
        }
    }
}

QString StandalonePluginFinder::buildCommaSeparetedString(QStringList list) const
{
    QString folderString;
    for (int c = 0; c < list.size(); ++c) {
        folderString += list.at(c);
        if (c < list.size() -1)
            folderString += ";";
    }
    return folderString;
}

void StandalonePluginFinder::scan(QStringList skipList)
{
    if (scanProcess.isOpen()) {
        qCWarning(jtStandalonePluginFinder) << "scan process is already open!";
        return;
    }

    QString scannerExePath = getVstScannerExecutablePath();
    if (scannerExePath.isEmpty())
        return;// scanner executable not found!

    emit scanStarted();
    // execute the scanner in another process to avoid crash Jamtaba process
    QStringList parameters;
    parameters.append(buildCommaSeparetedString(scanFolders));
    parameters.append(buildCommaSeparetedString(skipList));
    QObject::connect(&scanProcess, SIGNAL(readyReadStandardOutput()), this,
                     SLOT(on_processStandardOutputReady()));
    QObject::connect(&scanProcess, SIGNAL(finished(int)), this, SLOT(on_processFinished()));
    QObject::connect(&scanProcess, SIGNAL(error(QProcess::ProcessError)), this,
                     SLOT(on_processError(QProcess::ProcessError)));
    qCDebug(jtStandalonePluginFinder) << "Starting scan process...";
    scanProcess.start(scannerExePath, parameters);
    qCDebug(jtStandalonePluginFinder) << "Scan process started with " << scannerExePath;
}

// ++++++++++++++++++++++++++++++++++

QString StandaloneMainController::getJamtabaFlavor() const
{
    return "Standalone";
}

// ++++++++++++++++++++++++++++++++++

void StandaloneMainController::setInputTrackToMono(int localChannelIndex,
                                                   int inputIndexInAudioDevice)
{
    Audio::LocalInputAudioNode *inputTrack = getInputTrack(localChannelIndex);
    if (inputTrack) {
        if (!inputIndexIsValid(inputIndexInAudioDevice))  // use the first available channel?
            inputIndexInAudioDevice = audioDriver->getSelectedInputs().getFirstChannel();

        inputTrack->setAudioInputSelection(inputIndexInAudioDevice, 1);// mono
        if (window) {
            window->refreshTrackInputSelection(
                localChannelIndex);
        }
        if (isPlayingInNinjamRoom()) {
            if (ninjamController)// just in case
                ninjamController->scheduleEncoderChangeForChannel(inputTrack->getGroupChannelIndex());

        }
    }
}

bool StandaloneMainController::inputIndexIsValid(int inputIndex)
{
    Audio::ChannelRange globalInputsRange = audioDriver->getSelectedInputs();
    return inputIndex >= globalInputsRange.getFirstChannel()
           && inputIndex <= globalInputsRange.getLastChannel();
}

void StandaloneMainController::setInputTrackToMIDI(int localChannelIndex, int midiDevice,
                                                   int midiChannel)
{
    Audio::LocalInputAudioNode *inputTrack = getInputTrack(localChannelIndex);
    if (inputTrack) {
        inputTrack->setMidiInputSelection(midiDevice, midiChannel);
        if (window)
            window->refreshTrackInputSelection(localChannelIndex);
        if (isPlayingInNinjamRoom()) {
            if (ninjamController)
                ninjamController->scheduleEncoderChangeForChannel(inputTrack->getGroupChannelIndex());

        }
    }
}

void StandaloneMainController::setInputTrackToNoInput(int localChannelIndex)
{
    Audio::LocalInputAudioNode *inputTrack = getInputTrack(localChannelIndex);
    if (inputTrack) {
        inputTrack->setToNoInput();
        if (window)
            window->refreshTrackInputSelection(localChannelIndex);
        if (isPlayingInNinjamRoom()) {// send the finish interval message
            if (intervalsToUpload.contains(localChannelIndex)) {
                ninjamService.sendAudioIntervalPart(
                    intervalsToUpload[localChannelIndex]->getGUID(), QByteArray(), true);
                if (ninjamController)
                    ninjamController->scheduleEncoderChangeForChannel(
                        inputTrack->getGroupChannelIndex());
            }
        }
    }
}

void StandaloneMainController::setInputTrackToStereo(int localChannelIndex, int firstInputIndex)
{
    Audio::LocalInputAudioNode *inputTrack = getInputTrack(localChannelIndex);
    if (inputTrack) {
        if (!inputIndexIsValid(firstInputIndex))
            firstInputIndex = audioDriver->getSelectedInputs().getFirstChannel();// use the first available channel
        inputTrack->setAudioInputSelection(firstInputIndex, 2);// stereo

        if (window)
            window->refreshTrackInputSelection(localChannelIndex);
        if (isPlayingInNinjamRoom()) {
            if (ninjamController)
                ninjamController->scheduleEncoderChangeForChannel(inputTrack->getGroupChannelIndex());

        }
    }
}

void StandaloneMainController::updateBpm(int newBpm)
{
    MainController::updateBpm(newBpm);
    vstHost->setTempo(newBpm);
}

void StandaloneMainController::connectedNinjamServer(Ninjam::Server server)
{
    MainController::connectedNinjamServer(server);
    vstHost->setTempo(server.getBpm());
}

void StandaloneMainController::setSampleRate(int newSampleRate)
{
    MainController::setSampleRate(newSampleRate);
    vstHost->setSampleRate(newSampleRate);
    foreach (Audio::LocalInputAudioNode *inputNode, inputTracks)
        inputNode->setProcessorsSampleRate(newSampleRate);
}

void StandaloneMainController::on_audioDriverStarted()
{
    //MainController::on_audioDriverStarted();

    vstHost->setSampleRate(audioDriver->getSampleRate());
    vstHost->setBlockSize(audioDriver->getBufferSize());

    foreach (Audio::LocalInputAudioNode *inputTrack, inputTracks)
        inputTrack->resumeProcessors();
}

void StandaloneMainController::on_audioDriverStopped()
{
    MainController::on_audioDriverStopped();
    foreach (Audio::LocalInputAudioNode *inputTrack, inputTracks)
        inputTrack->suspendProcessors();// suspend plugins
}

void StandaloneMainController::on_newNinjamInterval()
{
    MainController::on_newNinjamInterval();
    vstHost->setPlayingFlag(true);
}

void StandaloneMainController::on_ninjamStartProcessing(int intervalPosition)
{
    MainController::on_ninjamStartProcessing(intervalPosition);
    vstHost->update(intervalPosition);// update the vst host time line.
}

void StandaloneMainController::on_VSTPluginFounded(QString name, QString group, QString path)
{
    pluginsDescriptors.append(Audio::PluginDescriptor(name, group, path));
    settings.addVstPlugin(path);
}

// ++++++++++++++++++++++++++++++++++++++++++

bool StandaloneMainController::isRunningAsVstPlugin() const
{
    return false;
}

Vst::PluginFinder *StandaloneMainController::createPluginFinder()
{
    return new StandalonePluginFinder();
}

void StandalonePluginFinder::cancel()
{
    if (scanProcess.isOpen())
        scanProcess.terminate();
}

void StandaloneMainController::setMainWindow(MainWindow *mainWindow)
{
    MainController::setMainWindow(mainWindow);

    // store a casted pointer to convenience when callen MainWindowStandalone specific functions
    window = dynamic_cast<MainWindowStandalone *>(mainWindow);
}

Midi::MidiDriver *StandaloneMainController::createMidiDriver()
{
    // return new Midi::PortMidiDriver(settings.getMidiInputDevicesStatus());
    return new Midi::RtMidiDriver(settings.getMidiInputDevicesStatus());
    // return new Midi::NullMidiDriver();
}

Controller::NinjamController *StandaloneMainController::createNinjamController(MainController *c)
{
    return new NinjamController(c);
}

Audio::AudioDriver *StandaloneMainController::createAudioDriver(
    const Persistence::Settings &settings)
{
    return new Audio::PortAudioDriver(
        this,
        settings.getLastAudioDevice(),
        settings.getFirstGlobalAudioInput(),
        settings.getLastGlobalAudioInput(),
        settings.getFirstGlobalAudioOutput(),
        settings.getLastGlobalAudioOutput(),
        settings.getLastSampleRate(),
        settings.getLastBufferSize()
        );
}

// ++++++++++++++++++++++++++++++++++++++++++
StandaloneMainController::StandaloneMainController(Persistence::Settings settings,
                                                   QApplication *application) :
    MainController(settings),
    vstHost(Vst::Host::getInstance()),
    application(application)
{
    application->setQuitOnLastWindowClosed(true);

    QObject::connect(Vst::Host::getInstance(),
                     SIGNAL(pluginRequestingWindowResize(QString, int, int)),
                     this, SLOT(on_vstPluginRequestedWindowResize(QString, int, int)));
}

void StandaloneMainController::on_vstPluginRequestedWindowResize(QString pluginName, int newWidht,
                                                                 int newHeight)
{
    QDialog *pluginEditorWindow = Vst::VstPlugin::getPluginEditorWindow(pluginName);
    if (pluginEditorWindow) {
        pluginEditorWindow->setFixedSize(newWidht, newHeight);
        // pluginEditorWindow->updateGeometry();
    }
}

void StandaloneMainController::start()
{
    // creating audio and midi driver before call the base class MainController::start()

    if (!midiDriver) {
        qCInfo(jtCore) << "Creating midi driver...";
        midiDriver.reset(createMidiDriver());
    }
    if (!audioDriver) {
        qCInfo(jtCore) << "Creating audio driver...";
        Audio::AudioDriver *driver = nullptr;
        try{
            driver = createAudioDriver(settings);
        }
        catch (const std::runtime_error &error) {
            qCCritical(jtCore) << "Audio initialization fail: " << QString::fromUtf8(
                error.what());
            QMessageBox::warning(window, "Audio Initialization Problem!", error.what());
        }
        if (!driver)
            driver = new Audio::NullAudioDriver();

        audioDriver.reset(driver);

        QObject::connect(audioDriver.data(), SIGNAL(sampleRateChanged(int)), this,
                         SLOT(setSampleRate(int)));
        QObject::connect(audioDriver.data(), SIGNAL(stopped()), this,
                         SLOT(on_audioDriverStopped()));
        QObject::connect(audioDriver.data(), SIGNAL(started()), this,
                         SLOT(on_audioDriverStarted()));
    }

    MainController::start();

    if (audioDriver) {
        if (!audioDriver->canBeStarted())
            useNullAudioDriver();
        audioDriver->start();
    }
    if (midiDriver)
        midiDriver->start();

    QObject::connect(pluginFinder.data(), SIGNAL(pluginScanFinished(QString, QString,
                                                                    QString)), this,
                     SLOT(on_VSTPluginFounded(QString, QString, QString)));

    if (audioDriver) {
        vstHost->setSampleRate(audioDriver->getSampleRate());
        vstHost->setBlockSize(audioDriver->getBufferSize());
    }
}

void StandaloneMainController::setCSS(QString css)
{
    application->setStyleSheet(css);
}

StandaloneMainController::~StandaloneMainController()
{
    qCDebug(jtCore) << "StandaloneMainController destructor!";
    // pluginsDescriptors.clear();
}

Audio::Plugin *StandaloneMainController::createPluginInstance(
    const Audio::PluginDescriptor &descriptor)
{
    if (descriptor.isNative()) {
        if (descriptor.getName() == "Delay")
            return new Audio::JamtabaDelay(audioDriver->getSampleRate());
    } else if (descriptor.isVST()) {
        Vst::VstPlugin *vstPlugin = new Vst::VstPlugin(this->vstHost);
        if (vstPlugin->load(descriptor.getPath()))
            return vstPlugin;
    }
    return nullptr;
}

QStringList StandaloneMainController::getSteinbergRecommendedPaths()
{
    /*
    On a 64-bit OS

    64-bit plugins path = HKEY_LOCAL_MACHINE\SOFTWARE\VST
    32-bit plugins path = HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\VST
    */
    QStringList vstPaths;
#ifdef Q_OS_WIN

    #ifdef _WIN64// 64 bits
    QSettings wowSettings("HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\VST\\",
                          QSettings::NativeFormat);
    vstPaths.append(wowSettings.value("VSTPluginsPath").toString());
    #else// 32 bits
    QSettings settings("HKEY_LOCAL_MACHINE\\SOFTWARE\\VST\\", QSettings::NativeFormat);
    vstPaths.append(settings.value("VSTPluginsPath").toString());
    #endif
#endif

#ifdef Q_OS_MACX
    vstPaths.append("/Library/Audio/Plug-Ins/VST");
    vstPaths.append("~/Library/Audio/Plug-Ins/VST");
#endif
    return vstPaths;
}

void StandaloneMainController::addDefaultPluginsScanPath()
{
    QStringList vstPaths;

    // first try read the path store in registry by Jamtaba installer.
    // If this value is not founded use the Steinberg recommended paths.
    QSettings jamtabaRegistryEntry("HKEY_CURRENT_USER\\SOFTWARE\\Jamtaba", QSettings::NativeFormat);
    QString vst2InstallDir = jamtabaRegistryEntry.value("VST2InstallDir").toString();
    if (!vst2InstallDir.isEmpty())
        vstPaths.append(vst2InstallDir);
    else
        vstPaths.append(getSteinbergRecommendedPaths());

    foreach (QString vstPath, vstPaths) {
        if (!vstPath.isEmpty() && QDir(vstPath).exists())
            addPluginsScanPath(vstPath);
    }
}

// we need scan plugins when plugins cache is empty OR we have new plugins in scan folders
// This code is executed whem Jamtaba is started.
bool StandaloneMainController::pluginsScanIsNeeded() const
{
    if (settings.getVstPluginsPaths().isEmpty())// cache is empty?

        return true;

    // checking for new plugins in scan folders
    QStringList foldersToScan = settings.getVstScanFolders();

    QStringList skipList(settings.getBlackListedPlugins());
    skipList.append(settings.getVstPluginsPaths());

    foreach (QString scanFolder, foldersToScan) {
        QDirIterator folderIterator(scanFolder, QDirIterator::Subdirectories);
        while (folderIterator.hasNext())
        {
            folderIterator.next();// point to next file inside current folder
            QString filePath = folderIterator.filePath();
            if (isVstPluginFile(filePath) && !skipList.contains(filePath))
                return true; // a new vst plugin was founded
        }
    }
    return false;
}

bool StandaloneMainController::isVstPluginFile(QString filePath) const
{
#ifdef Q_OS_WIN
    return QLibrary::isLibrary(filePath);
#endif

#ifdef Q_OS_MAC
    QFileInfo file(filePath);
    return file.isBundle() && file.absoluteFilePath().endsWith(".vst");
#endif
    return false; // just in case
}

void StandaloneMainController::initializePluginsList(QStringList paths)
{
    pluginsDescriptors.clear();
    foreach (QString path, paths) {
        QFile file(path);
        if (file.exists()) {
            QString pluginName = Audio::PluginDescriptor::getPluginNameFromPath(path);
            pluginsDescriptors.append(Audio::PluginDescriptor(pluginName, "VST", path));
        }
    }
}

void StandaloneMainController::scanPlugins(bool scanOnlyNewPlugins)
{
    if (pluginFinder) {
        if (!scanOnlyNewPlugins)
            pluginsDescriptors.clear();

        pluginFinder->setFoldersToScan(settings.getVstScanFolders());

        // The skipList contains the paths for black listed plugins by default.
        // If the parameter 'scanOnlyNewPlugins' is 'true' the cached plugins are added in the skipList too.
        QStringList skipList(settings.getBlackListedPlugins());
        if (scanOnlyNewPlugins)
            skipList.append(settings.getVstPluginsPaths());
        pluginFinder->scan(skipList);
    }
}

void StandaloneMainController::stopNinjamController()
{
    MainController::stopNinjamController();
    vstHost->setPlayingFlag(false);
}

void StandaloneMainController::quit()
{
    // destroy the extern !
    // if(JTBConfig)delete JTBConfig;
    qDebug() << "Thank you for Jamming with Jamtaba !";
    application->quit();
}

Midi::MidiBuffer StandaloneMainController::pullMidiBuffer()
{
    Midi::MidiBuffer midiBuffer(midiDriver ? midiDriver->getBuffer() : Midi::MidiBuffer(0));
// int messages = midiBuffer.getMessagesCount();
// for(int m=0; m < messages; m++){
// Midi::MidiMessage msg = midiBuffer.getMessage(m);
// if(msg.isControl()){
// int inputTrackIndex = 0;//just for test for while, we need get this index from the mapping pair
// char cc = msg.getData1();
// char ccValue = msg.getData2();
// qCDebug(jtMidi) << "Control Change received: " << QString::number(cc) << " -> " << QString::number(ccValue);
// getInputTrack(inputTrackIndex)->setGain(ccValue/127.0);
// }
// }
    return midiBuffer;
}

bool StandaloneMainController::isUsingNullAudioDriver() const
{
    return qobject_cast<Audio::NullAudioDriver *>(audioDriver.data()) != nullptr;
}

void StandaloneMainController::stop()
{
    MainController::stop();
    if (audioDriver)
        this->audioDriver->release();
    if (midiDriver)
        this->midiDriver->release();
    qCDebug(jtCore) << "audio and midi drivers released";
}

void StandaloneMainController::useNullAudioDriver()
{
    qCWarning(jtCore) << "Audio driver can't be used, using NullAudioDriver!";
    audioDriver.reset(new Audio::NullAudioDriver());
}

void StandaloneMainController::updateInputTracksRange()
{
    Audio::ChannelRange globalInputRange = audioDriver->getSelectedInputs();

    for (int trackIndex = 0; trackIndex < inputTracks.size(); ++trackIndex) {
        Audio::LocalInputAudioNode *inputTrack = getInputTrack(trackIndex);

        if (!inputTrack->isNoInput()) {
            if (inputTrack->isAudio()) {// audio track
                Audio::ChannelRange inputTrackRange = inputTrack->getAudioInputRange();
                inputTrack->setGlobalFirstInputIndex(globalInputRange.getFirstChannel());

                /** If global input range is reduced to 2 channels and user previous selected inputs 3+4 the input range need be corrected to avoid a beautiful crash :) */
                if (globalInputRange.getChannels() < inputTrackRange.getChannels()) {
                    if (globalInputRange.isMono())
                        setInputTrackToMono(trackIndex, globalInputRange.getFirstChannel());
                    else
                        setInputTrackToNoInput(trackIndex);
                }

                // check if localInputRange is valid after the change in globalInputRange
                int firstChannel = inputTrackRange.getFirstChannel();
                int globalFirstChannel = globalInputRange.getFirstChannel();
                if (firstChannel < globalFirstChannel
                    || inputTrackRange.getLastChannel() > globalInputRange.getLastChannel()) {
                    if (globalInputRange.isMono())
                        setInputTrackToMono(trackIndex, globalInputRange.getFirstChannel());
                    else if (globalInputRange.getChannels() >= 2)
                        setInputTrackToStereo(trackIndex, globalInputRange.getFirstChannel());
                }
            } else {// midi track
                int selectedDevice = inputTrack->getMidiDeviceIndex();
                bool deviceIsValid = selectedDevice >= 0
                                     && selectedDevice < midiDriver->getMaxInputDevices()
                                     && midiDriver->deviceIsGloballyEnabled(selectedDevice);
                if (!deviceIsValid) {
                    // try another available midi input device
                    int firstAvailableDevice = midiDriver->getFirstGloballyEnableInputDevice();
                    if (firstAvailableDevice >= 0)
                        setInputTrackToMIDI(trackIndex, firstAvailableDevice, -1);// select all channels
                    else
                        setInputTrackToNoInput(trackIndex);
                }
            }
        }
    }
}
