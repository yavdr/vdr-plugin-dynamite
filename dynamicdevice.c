#include "dynamicdevice.h"
#include <glob.h>
#include <vdr/transfer.h>

cPlugin *cDynamicDevice::dynamite = NULL;
int cDynamicDevice::defaultGetTSTimeout = 0;
cDvbDeviceProbe *cDynamicDevice::dvbprobe = NULL;
int cDynamicDevice::numDynamicDevices = 0;
cMutex cDynamicDevice::arrayMutex;
cDynamicDevice *cDynamicDevice::dynamicdevice[MAXDEVICES] = { NULL };

int cDynamicDevice::IndexOf(const char *DevPath, int &NextFreeIndex)
{
  cMutexLock lock(&arrayMutex);
  NextFreeIndex = -1;
  int index = -1;
  for (int i = 0; ((index < 0) || (NextFreeIndex < 0)) && (i < numDynamicDevices); i++) {
      if (dynamicdevice[i]->devpath == NULL) {
         if (NextFreeIndex < 0)
            NextFreeIndex = i;
         }
      else if (index < 0) {
         if (strcmp(DevPath, **dynamicdevice[i]->devpath) == 0)
            index = i;
         }
      }
  return index;
}

bool cDynamicDevice::ProcessQueuedCommands(void)
{
  for (cDynamicDeviceProbe::cDynamicDeviceProbeItem *dev = cDynamicDeviceProbe::commandQueue.First(); dev; dev = cDynamicDeviceProbe::commandQueue.Next(dev)) {
      switch (dev->cmd) {
         case ddpcAttach:
          {
           AttachDevice(*dev->devpath);
           break;
          }
         case ddpcDetach:
          {
           DetachDevice(*dev->devpath);
           break;
          }
         case ddpcService:
          {
           if (dynamite && (dev->devpath != NULL) && (**dev->devpath != NULL)) {
              int len = strlen(*dev->devpath);
              if (len > 0) {
                 char *data = strchr(const_cast<char*>(**dev->devpath), ' ');
                 if (data != NULL) {
                    data[0] = '\0';
                    data++;
                    dynamite->Service(*dev->devpath, data);
                    }
                 }
              }
           break;
          }
        }
      }
  cDynamicDeviceProbe::commandQueue.Clear();
  return true;
}

void cDynamicDevice::DetachAllDevices(void)
{
  cMutexLock lock(&arrayMutex);
  for (int i = 0; i < numDynamicDevices; i++)
      dynamicdevice[i]->DeleteSubDevice();
}

cString cDynamicDevice::ListAllDevices(int &ReplyCode)
{
  cMutexLock lock(&arrayMutex);
  cString devices;
  int count = 0;
  for (int i = 0; i < numDynamicDevices; i++) {
      if ((dynamicdevice[i]->devpath != NULL) && (dynamicdevice[i]->subDevice != NULL)) {
         count++;
         devices = cString::sprintf("%s%d%s %s\n", (count == 1) ? "" : *devices
                                                 , i + 1
                                                 , ((PrimaryDevice() == dynamicdevice[i]) || !dynamicdevice[i]->isDetachable) ? "*" : ""
                                                 , **dynamicdevice[i]->devpath);
         }
      }
  if (count == 0) {
     ReplyCode = 901;
     return cString::sprintf("there are no attached devices");
     }
  return devices;
}

cString cDynamicDevice::AttachDevicePattern(const char *Pattern)
{
  if (!Pattern)
     return "invalid pattern";
  cString reply;
  glob_t result;
  if (glob(Pattern, GLOB_MARK, 0, &result) == 0) {
     for (uint i = 0; i < result.gl_pathc; i++) {
         cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcAttach, result.gl_pathv[i]);
         reply = cString::sprintf("%squeued %s for attaching\n", (i == 0) ? "" : *reply, result.gl_pathv[i]);
         }
     }
  globfree(&result);
  return reply;
}

eDynamicDeviceReturnCode cDynamicDevice::AttachDevice(const char *DevPath)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = IndexOf(DevPath, freeIndex);
  int adapter = -1;
  int frontend = -1;

  if (index >= 0) {
     esyslog("dynamite: %s is already attached", DevPath);
     return ddrcAlreadyAttached;
     }

  if (freeIndex < 0) {
     esyslog("dynamite: no more free slots for %s", DevPath);
     return ddrcNoFreeDynDev;
     }

  cDevice::nextParentDevice = dynamicdevice[freeIndex];
  
  for (cDynamicDeviceProbe *ddp = DynamicDeviceProbes.First(); ddp; ddp = DynamicDeviceProbes.Next(ddp)) {
      if (ddp->Attach(dynamicdevice[freeIndex], DevPath))
         goto attach; // a plugin has created the actual device
      }

  // if it's a dvbdevice try the DvbDeviceProbes as a fallback for unpatched plugins
  if (sscanf(DevPath, "/dev/dvb/adapter%d/frontend%d", &adapter, &frontend) == 2) {
     for (cDvbDeviceProbe *dp = DvbDeviceProbes.First(); dp; dp = DvbDeviceProbes.Next(dp)) {
         if (dp != dvbprobe) {
            if (dp->Probe(adapter, frontend))
               goto attach;
            }
         }
     new cDvbDevice(adapter, frontend, dynamicdevice[freeIndex]);
     goto attach;
     }

  esyslog("dynamite: can't attach %s", DevPath);
  return ddrcNotSupported;

attach:
  while (!dynamicdevice[freeIndex]->Ready())
        cCondWait::SleepMs(2);
  dynamicdevice[freeIndex]->devpath = new cString(DevPath);
  isyslog("dynamite: attached device %s to dynamic device slot %d", DevPath, freeIndex + 1);
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::DetachDevice(const char *DevPath)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex);

  if ((index < 0) || (index >= numDynamicDevices)) {
     esyslog("dynamite: device %s not found", DevPath);
     return ddrcNotFound;
     }

  if (!dynamicdevice[index]->isDetachable) {
     esyslog("dynamite: detaching of device %s is not allowed", DevPath);
     return ddrcNotAllowed;
     }

  if (dynamicdevice[index] == PrimaryDevice()) {
     esyslog("dynamite: detaching of primary device %s is not supported", DevPath);
     return ddrcIsPrimaryDevice;
     }

  if (dynamicdevice[index]->Receiving(false)) {
     esyslog("dynamite: can't detach device %s, it's receiving something important", DevPath);
     return ddrcIsReceiving;
     }

  dynamicdevice[index]->DeleteSubDevice();
  isyslog("dynamite: detached device %s", DevPath);
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::SetLockDevice(const char *DevPath, bool Lock)
{
  if (!DevPath)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  dynamicdevice[index]->isDetachable = !Lock;
  isyslog("dynamite: %slocked device %s", Lock ? "" : "un", DevPath);
  return ddrcSuccess;
}

eDynamicDeviceReturnCode cDynamicDevice::SetGetTSTimeout(const char *DevPath, int Seconds)
{
  if (!DevPath || (Seconds < 0))
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  dynamicdevice[index]->getTSTimeout = Seconds;
  if (Seconds == 0)
     isyslog("dynamite: disable GetTSTimeout on device %s", DevPath);
  else
     isyslog("dynamite: set GetTSTimeout on device %s to %d seconds", DevPath, Seconds);
  return ddrcSuccess;
}

void cDynamicDevice::SetDefaultGetTSTimeout(int Seconds)
{
  if (Seconds >= 0) {
     defaultGetTSTimeout = Seconds;
     cMutexLock lock(&arrayMutex);
     for (int i = 0; i < numDynamicDevices; i++)
         dynamicdevice[i]->getTSTimeout = Seconds;
     isyslog("dynamite: set default GetTSTimeout on all devices to %d seconds", Seconds);
     }
}

eDynamicDeviceReturnCode cDynamicDevice::SetGetTSTimeoutHandlerArg(const char *DevPath, const char *Arg)
{
  if (!DevPath || !Arg)
     return ddrcNotSupported;

  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = -1;
  if (isnumber(DevPath))
     index = strtol(DevPath, NULL, 10) - 1;
  else
     index = IndexOf(DevPath, freeIndex);

  if ((index < 0) || (index >= numDynamicDevices))
     return ddrcNotFound;

  if (dynamicdevice[index]->getTSTimeoutHandlerArg)
     delete dynamicdevice[index]->getTSTimeoutHandlerArg;
  dynamicdevice[index]->getTSTimeoutHandlerArg = new cString(Arg);
  isyslog("dynamite: set GetTSTimeoutHandlerArg on device %s to %s", DevPath, Arg);
  return ddrcSuccess;
}

bool cDynamicDevice::IsAttached(const char *DevPath)
{
  cMutexLock lock(&arrayMutex);
  int freeIndex = -1;
  int index = IndexOf(DevPath, freeIndex);
  return ((index >= 0) && (index >= numDynamicDevices));
}

cDynamicDevice::cDynamicDevice()
:index(-1)
,devpath(NULL)
,getTSTimeoutHandlerArg(NULL)
,isDetachable(true)
,getTSTimeout(defaultGetTSTimeout)
,restartSectionHandler(false)
{
  index = numDynamicDevices;
  if (numDynamicDevices < MAXDEVICES) {
     dynamicdevice[index] = this;
     numDynamicDevices++;
     }
  else
     esyslog("dynamite: ERROR: too many dynamic devices!");
}

cDynamicDevice::~cDynamicDevice()
{
  DeleteSubDevice();
  if (getTSTimeoutHandlerArg)
     delete getTSTimeoutHandlerArg;
  getTSTimeoutHandlerArg = NULL;
}

void cDynamicDevice::DeleteSubDevice()
{
  if (subDevice) {
     Cancel(3);
     if (cTransferControl::ReceiverDevice() == this)
        cControl::Shutdown();
     subDevice->Detach(player);
     subDevice->DetachAllReceivers();
     subDevice->StopSectionHandler();
     delete subDevice;
     subDevice = NULL;
     }
  if (devpath) {
     delete devpath;
     devpath = NULL;
     }
  isDetachable = true;
  getTSTimeout = defaultGetTSTimeout;
}

bool cDynamicDevice::SetIdleDevice(bool Idle, bool TestOnly)
{
  if (subDevice)
     return subDevice->SetIdleDevice(Idle, TestOnly);
  return false;
}

bool cDynamicDevice::CanScanForEPG(void) const
{
  if (subDevice)
     return subDevice->CanScanForEPG();
  return false;
}

void cDynamicDevice::MakePrimaryDevice(bool On)
{
  if (subDevice)
     subDevice->MakePrimaryDevice(On);
  cDevice::MakePrimaryDevice(On);
}

bool cDynamicDevice::HasDecoder(void) const
{
  if (subDevice)
     return subDevice->HasDecoder();
  return cDevice::HasDecoder();
}

cSpuDecoder *cDynamicDevice::GetSpuDecoder(void)
{
  if (subDevice)
     return subDevice->GetSpuDecoder();
  return cDevice::GetSpuDecoder();
}

bool cDynamicDevice::HasCi(void)
{
  if (subDevice)
     return subDevice->HasCi();
  return cDevice::HasCi();
}

uchar *cDynamicDevice::GrabImage(int &Size, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  if (subDevice)
     return subDevice->GrabImage(Size, Jpeg, Quality, SizeX, SizeY);
  return cDevice::GrabImage(Size, Jpeg, Quality, SizeX, SizeY);
}

void cDynamicDevice::SetVideoDisplayFormat(eVideoDisplayFormat VideoDisplayFormat)
{
  if (subDevice)
     return subDevice->SetVideoDisplayFormat(VideoDisplayFormat);
  cDevice::SetVideoDisplayFormat(VideoDisplayFormat);
}

void cDynamicDevice::SetVideoFormat(bool VideoFormat16_9)
{
  if (subDevice)
     return subDevice->SetVideoFormat(VideoFormat16_9);
  cDevice::SetVideoFormat(VideoFormat16_9);
}

eVideoSystem cDynamicDevice::GetVideoSystem(void)
{
  if (subDevice)
     return subDevice->GetVideoSystem();
  return cDevice::GetVideoSystem();
}

void cDynamicDevice::GetVideoSize(int &Width, int &Height, double &VideoAspect)
{
  if (subDevice)
     return subDevice->GetVideoSize(Width, Height, VideoAspect);
  cDevice::GetVideoSize(Width, Height, VideoAspect);
}

void cDynamicDevice::GetOsdSize(int &Width, int &Height, double &PixelAspect)
{
  if (subDevice)
     return subDevice->GetOsdSize(Width, Height, PixelAspect);
  cDevice::GetOsdSize(Width, Height, PixelAspect);
}

bool cDynamicDevice::SetPid(cPidHandle *Handle, int Type, bool On)
{
  if (subDevice)
     return subDevice->SetPid(Handle, Type, On);
  return cDevice::SetPid(Handle, Type, On);
}

int cDynamicDevice::OpenFilter(u_short Pid, u_char Tid, u_char Mask)
{
  if (subDevice)
     return subDevice->OpenFilter(Pid, Tid, Mask);
  return cDevice::OpenFilter(Pid, Tid, Mask);
}

void cDynamicDevice::CloseFilter(int Handle)
{
  if (subDevice)
     return subDevice->CloseFilter(Handle);
  cDevice::CloseFilter(Handle);
}

bool cDynamicDevice::ProvidesSource(int Source) const
{
  if (subDevice)
     return subDevice->ProvidesSource(Source);
  return cDevice::ProvidesSource(Source);
}

bool cDynamicDevice::ProvidesTransponder(const cChannel *Channel) const
{
  if (subDevice)
     return subDevice->ProvidesTransponder(Channel);
  return cDevice::ProvidesTransponder(Channel);
}

bool cDynamicDevice::ProvidesTransponderExclusively(const cChannel *Channel) const
{
  if (subDevice)
     return subDevice->ProvidesTransponderExclusively(Channel);
  return cDevice::ProvidesTransponderExclusively(Channel);
}

bool cDynamicDevice::ProvidesChannel(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers) const
{
  if (subDevice)
     return subDevice->ProvidesChannel(Channel, Priority, NeedsDetachReceivers);
  return cDevice::ProvidesChannel(Channel, Priority, NeedsDetachReceivers);
}

int cDynamicDevice::NumProvidedSystems(void) const
{
  if (subDevice)
     return subDevice->NumProvidedSystems();
  return cDevice::NumProvidedSystems();
}

const cChannel *cDynamicDevice::GetCurrentlyTunedTransponder(void) const
{
  if (subDevice)
     return subDevice->GetCurrentlyTunedTransponder();
  return cDevice::GetCurrentlyTunedTransponder();
}

bool cDynamicDevice::IsTunedToTransponder(const cChannel *Channel)
{
  if (subDevice)
     return subDevice->IsTunedToTransponder(Channel);
  return cDevice::IsTunedToTransponder(Channel);
}

bool cDynamicDevice::MaySwitchTransponder(void)
{
  if (subDevice)
     return subDevice->MaySwitchTransponder();
  return cDevice::MaySwitchTransponder();
}

bool cDynamicDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
  if (subDevice)
     return subDevice->SetChannelDevice(Channel, LiveView);
  return cDevice::SetChannelDevice(Channel, LiveView);
}

bool cDynamicDevice::HasLock(int TimeoutMs)
{
  if (subDevice)
     return subDevice->HasLock(TimeoutMs);
  return cDevice::HasLock(TimeoutMs);
}

bool cDynamicDevice::HasProgramme(void)
{
  if (subDevice)
     return subDevice->HasProgramme();
  return cDevice::HasProgramme();
}

int cDynamicDevice::GetAudioChannelDevice(void)
{
  if (subDevice)
     return subDevice->GetAudioChannelDevice();
  return GetAudioChannelDevice();
}

void cDynamicDevice::SetAudioChannelDevice(int AudioChannel)
{
  if (subDevice)
     return subDevice->SetAudioChannelDevice(AudioChannel);
  cDevice::SetAudioChannelDevice(AudioChannel);
}

void cDynamicDevice::SetVolumeDevice(int Volume)
{
  if (subDevice)
     return subDevice->SetVolumeDevice(Volume);
  cDevice::SetVolumeDevice(Volume);
}

void cDynamicDevice::SetDigitalAudioDevice(bool On)
{
  if (subDevice)
     return subDevice->SetDigitalAudioDevice(On);
  cDevice::SetDigitalAudioDevice(On);
}

void cDynamicDevice::SetAudioTrackDevice(eTrackType Type)
{
  if (subDevice)
     return subDevice->SetAudioTrackDevice(Type);
  cDevice::SetAudioTrackDevice(Type);
}

void cDynamicDevice::SetSubtitleTrackDevice(eTrackType Type)
{
  if (subDevice)
     return subDevice->SetSubtitleTrackDevice(Type);
  cDevice::SetSubtitleTrackDevice(Type);
}

bool cDynamicDevice::CanReplay(void) const
{
  if (subDevice)
     return subDevice->CanReplay();
  return cDevice::CanReplay();
}

bool cDynamicDevice::SetPlayMode(ePlayMode PlayMode)
{
  if (subDevice)
     return subDevice->SetPlayMode(PlayMode);
  return cDevice::SetPlayMode(PlayMode);
}

int64_t cDynamicDevice::GetSTC(void)
{
  if (subDevice)
     return subDevice->GetSTC();
  return cDevice::GetSTC();
}

bool cDynamicDevice::IsPlayingVideo(void) const
{
  if (subDevice)
     return subDevice->IsPlayingVideo();
  return cDevice::IsPlayingVideo();
}

bool cDynamicDevice::HasIBPTrickSpeed(void)
{
  if (subDevice)
     return subDevice->HasIBPTrickSpeed();
  return cDevice::HasIBPTrickSpeed();
}

void cDynamicDevice::TrickSpeed(int Speed)
{
  if (subDevice)
     return subDevice->TrickSpeed(Speed);
  cDevice::TrickSpeed(Speed);
}

void cDynamicDevice::Clear(void)
{
  if (subDevice)
     return subDevice->Clear();
  cDevice::Clear();
}

void cDynamicDevice::Play(void)
{
  if (subDevice)
     return subDevice->Play();
  cDevice::Play();
}

void cDynamicDevice::Freeze(void)
{
  if (subDevice)
     return subDevice->Freeze();
  cDevice::Freeze();
}

void cDynamicDevice::Mute(void)
{
  if (subDevice)
     return subDevice->Mute();
  cDevice::Mute();
}

void cDynamicDevice::StillPicture(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->StillPicture(Data, Length);
  cDevice::StillPicture(Data, Length);
}

bool cDynamicDevice::Poll(cPoller &Poller, int TimeoutMs)
{
  if (subDevice)
     return subDevice->Poll(Poller, TimeoutMs);
  return cDevice::Poll(Poller, TimeoutMs);
}

bool cDynamicDevice::Flush(int TimeoutMs)
{
  if (subDevice)
     return subDevice->Flush(TimeoutMs);
  return cDevice::Flush(TimeoutMs);
}

int cDynamicDevice::PlayVideo(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayVideo(Data, Length);
  return cDevice::PlayVideo(Data, Length);
}

int cDynamicDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  if (subDevice)
     return subDevice->PlayAudio(Data, Length, Id);
  return cDevice::PlayAudio(Data, Length, Id);
}

int cDynamicDevice::PlaySubtitle(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlaySubtitle(Data, Length);
  return cDevice::PlaySubtitle(Data, Length);
}

int cDynamicDevice::PlayPesPacket(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayPesPacket(Data, Length, VideoOnly);
  return cDevice::PlayPesPacket(Data, Length, VideoOnly);
}

int cDynamicDevice::PlayPes(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayPes(Data, Length, VideoOnly);
  return cDevice::PlayPes(Data, Length, VideoOnly);
}

int cDynamicDevice::PlayTsVideo(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsVideo(Data, Length);
  return cDevice::PlayTsVideo(Data, Length);
}

int cDynamicDevice::PlayTsAudio(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsAudio(Data, Length);
  return cDevice::PlayTsAudio(Data, Length);
}

int cDynamicDevice::PlayTsSubtitle(const uchar *Data, int Length)
{
  if (subDevice)
     return subDevice->PlayTsSubtitle(Data, Length);
  return cDevice::PlayTsSubtitle(Data, Length);
}

int cDynamicDevice::PlayTs(const uchar *Data, int Length, bool VideoOnly)
{
  if (subDevice)
     return subDevice->PlayTs(Data, Length, VideoOnly);
  return cDevice::PlayTs(Data, Length, VideoOnly);
}

bool cDynamicDevice::Ready(void)
{
  if (subDevice)
     return subDevice->Ready();
  return cDevice::Ready();
}

bool cDynamicDevice::OpenDvr(void)
{
  if (subDevice) {
     getTSWatchdog = 0;
     return subDevice->OpenDvr();
     }
  return cDevice::OpenDvr();
}

void cDynamicDevice::CloseDvr(void)
{
  if (subDevice)
     return subDevice->CloseDvr();
  cDevice::CloseDvr();
}

bool cDynamicDevice::GetTSPacket(uchar *&Data)
{
  if (subDevice) {
     bool r = subDevice->GetTSPacket(Data);
     if (getTSTimeout > 0) {
        if (Data == NULL) {
           if (getTSWatchdog == 0)
              getTSWatchdog = time(NULL);
           else if ((time(NULL) - getTSWatchdog) > getTSTimeout) {
              const char *d = NULL;
              if (devpath)
                 d = **devpath;
              esyslog("dynamite: device %s hasn't delivered any data for %d seconds, detaching all receivers", d, getTSTimeout);
              subDevice->DetachAllReceivers();
              cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcDetach, *devpath);
              const char *timeoutHandlerArg = *devpath;
              if (getTSTimeoutHandlerArg)
                 timeoutHandlerArg = **getTSTimeoutHandlerArg;
              cDynamicDeviceProbe::QueueDynamicDeviceCommand(ddpcService, *cString::sprintf("dynamite-CallGetTSTimeoutHandler-v0.1 %s", timeoutHandlerArg));
              return false;
              }
           }
        else
           getTSWatchdog = 0;
           }
     return r;
     }
  return cDevice::GetTSPacket(Data);
}

#ifdef YAVDR_PATCHES
//opt-21_internal-cam-devices.dpatch
bool cDynamicDevice::HasInternalCam(void)
{
  if (subDevice)
     return subDevice->HasInternalCam();
  return cDevice::HasInternalCam();
}

//opt-44_rotor.dpatch 
bool cDynamicDevice::SendDiseqcCmd(dvb_diseqc_master_cmd cmd)
{
  if (subDevice)
     return subDevice->SendDiseqcCmd(cmd);
  return cDevice::SendDiseqcCmd(cmd);
}

//opt-64_lnb-sharing.dpatch 
void cDynamicDevice::SetLnbNrFromSetup(void)
{
  if (subDevice)
     return subDevice->SetLnbNrFromSetup();
  cDevice::SetLnbNrFromSetup();
}

int cDynamicDevice::LnbNr(void) const
{
  if (subDevice)
     return subDevice->LnbNr();
  return cDevice::LnbNr();
}

bool cDynamicDevice::IsShareLnb(const cDevice *Device)
{
  if (subDevice)
     return subDevice->IsShareLnb(Device);
  return cDevice::IsShareLnb(Device);
}

bool cDynamicDevice::IsLnbConflict(const cChannel *Channel)
{
  if (subDevice)
     return subDevice->IsLnbConflict(Channel);
  return cDevice::IsLnbConflict(Channel);
}
#endif