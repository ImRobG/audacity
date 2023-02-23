/**********************************************************************

  Audacity: A Digital Audio Editor

  VSTWrapper.cpp

  Dominic Mazzoni

  Paul Licameli split from VSTEffect.cpp

  This class implements a VST Plug-in effect.  The plug-in must be
  loaded in a platform-specific way and passed into the constructor,
  but from here this class handles the interfacing.

*//********************************************************************/

#include "VSTWrapper.h"

#if USE_VST

#include <wx/log.h>
#include <wx/time.h>

#if defined(__WXMSW__)
#include <shlwapi.h>
#pragma comment(lib, "shlwapi")
#else
#include <dlfcn.h>
#endif

#if defined(__WXMAC__)
#include <wx/osx/core/private.h>
#endif

#include <cstring>

#include "FileNames.h"
#include "AudacityMessageBox.h"
#include "XMLFileReader.h"
#include "Base64.h"

static float reinterpretAsFloat(uint32_t x)
{
    static_assert(sizeof(float) == sizeof(uint32_t), "Cannot reinterpret uint32_t to float since sizes are different.");
    float f;
    std::memcpy(&f, &x, sizeof(float));
    return f;
}

static uint32_t reinterpretAsUint32(float f)
{
    static_assert(sizeof(float) == sizeof(uint32_t), "Cannot reinterpret float to uint32_t since sizes are different.");

    uint32_t x;
    std::memcpy(&x, &f, sizeof(uint32_t));
    return x;
}

typedef AEffect *(*vstPluginMain)(audioMasterCallback audioMaster);

intptr_t VSTWrapper::AudioMaster(AEffect * effect,
                                       int32_t opcode,
                                       int32_t index,
                                       intptr_t value,
                                       void * ptr,
                                       float opt)
{
   VSTWrapper* vst = (effect ? static_cast<VSTWrapper*>(effect->ptr2) : nullptr);

   // Handles operations during initialization...before VSTEffect has had a
   // chance to set its instance pointer.
   switch (opcode)
   {
      case audioMasterVersion:
         return (intptr_t) 2400;

      case audioMasterCurrentId:
         return vst->mCurrentEffectID;

      case audioMasterGetVendorString:
         strcpy((char *) ptr, "Audacity Team");    // Do not translate, max 64 + 1 for null terminator
         return 1;

      case audioMasterGetProductString:
         strcpy((char *) ptr, "Audacity");         // Do not translate, max 64 + 1 for null terminator
         return 1;

      case audioMasterGetVendorVersion:
         return (intptr_t) (AUDACITY_VERSION << 24 |
                            AUDACITY_RELEASE << 16 |
                            AUDACITY_REVISION << 8 |
                            AUDACITY_MODLEVEL);

      // Some (older) effects depend on an effIdle call when requested.  An
      // example is the Antress Modern plugins which uses the call to update
      // the editors display when the program (preset) changes.
      case audioMasterNeedIdle:
         if (vst)
         {
            vst->NeedIdle();
            return 1;
         }
         return 0;

      // We would normally get this if the effect editor is dipslayed and something "major"
      // has changed (like a program change) instead of multiple automation calls.
      // Since we don't do anything with the parameters while the editor is displayed,
      // there's no need for us to do anything.
      case audioMasterUpdateDisplay:
         if (vst)
         {
            vst->UpdateDisplay();
            return 1;
         }
         return 0;

      // Return the current time info.
      case audioMasterGetTime:
         if (vst)
         {
            return (intptr_t) vst->GetTimeInfo();
         }
         return 0;

      // Inputs, outputs, or initial delay has changed...all we care about is initial delay.
      case audioMasterIOChanged:
         if (vst)
         {
            vst->SetBufferDelay(effect->initialDelay);
            return 1;
         }
         return 0;

      case audioMasterGetSampleRate:
         if (vst)
         {
            return (intptr_t) vst->GetSampleRate();
         }
         return 0;

      case audioMasterIdle:
         wxYieldIfNeeded();
         return 1;

      case audioMasterGetCurrentProcessLevel:
         if (vst)
         {
            return vst->GetProcessLevel();
         }
         return 0;

      case audioMasterGetLanguage:
         return kVstLangEnglish;

      // We always replace, never accumulate
      case audioMasterWillReplaceOrAccumulate:
         return 1;

      // Resize the window to accommodate the effect size
      case audioMasterSizeWindow:
         if (vst)
         {
            vst->SizeWindow(index, value);
         }
         return 1;

      case audioMasterCanDo:
      {
         char *s = (char *) ptr;
         if (strcmp(s, "acceptIOChanges") == 0 ||
            strcmp(s, "sendVstTimeInfo") == 0 ||
            strcmp(s, "startStopProcess") == 0 ||
            strcmp(s, "shellCategory") == 0 ||
            strcmp(s, "sizeWindow") == 0)
         {
            return 1;
         }

#if defined(VST_DEBUG)
#if defined(__WXMSW__)
         wxLogDebug(wxT("VST canDo: %s"), wxString::FromAscii((char *)ptr));
#else
         wxPrintf(wxT("VST canDo: %s\n"), wxString::FromAscii((char *)ptr));
#endif
#endif

         return 0;
      }

      case audioMasterBeginEdit:
      case audioMasterEndEdit:
         return 0;

      case audioMasterAutomate:
         if (vst)
         {
            vst->Automate(index, opt);
         }
         return 0;

      // We're always connected (sort of)
      case audioMasterPinConnected:

      // We don't do MIDI yet
      case audioMasterWantMidi:
      case audioMasterProcessEvents:

         // Don't need to see any messages about these
         return 0;
   }

#if defined(VST_DEBUG)
#if defined(__WXMSW__)
   wxLogDebug(wxT("vst: %p opcode: %d index: %d value: %p ptr: %p opt: %f user: %p"),
              effect, (int) opcode, (int) index, (void *) value, ptr, opt, vst);
#else
   wxPrintf(wxT("vst: %p opcode: %d index: %d value: %p ptr: %p opt: %f user: %p\n"),
            effect, (int) opcode, (int) index, (void *) value, ptr, opt, vst);
#endif
#endif

   return 0;
}

#if !defined(__WXMSW__)
void VSTWrapper::ModuleDeleter::operator() (void* p) const
{
   if (p)
      dlclose(p);
}
#endif

#if defined(__WXMAC__)
void VSTWrapper::ResourceHandle::reset()
{
   if (mpHandle)
      CFBundleCloseBundleResourceMap(mpHandle, mNum);
   mpHandle = nullptr;
   mNum = 0;
}
#endif

#if 0
VSTEffect::VSTEffect(const PluginPath & path)
:  VSTWrapper(path)
{
   memset(&mTimeInfo, 0, sizeof(mTimeInfo));
   mTimeInfo.samplePos = 0.0;
   mTimeInfo.sampleRate = 44100.0;  // this is a bogus value, but it's only for the display
   mTimeInfo.nanoSeconds = wxGetUTCTimeMillis().ToDouble();
   mTimeInfo.tempo = 120.0;
   mTimeInfo.timeSigNumerator = 4;
   mTimeInfo.timeSigDenominator = 4;
   mTimeInfo.flags = kVstTempoValid | kVstNanosValid;
}

VSTEffect::~VSTEffect()
{
}

// ============================================================================
// ComponentInterface Implementation
// ============================================================================

PluginPath VSTEffect::GetPath() const
{
   return mPath;
}

ComponentInterfaceSymbol VSTEffect::GetSymbol() const
{
   return VSTWrapper::GetSymbol();
}

VendorSymbol VSTEffect::GetVendor() const
{
   return { mVendor };
}

wxString VSTEffect::GetVersion() const
{
   wxString version;

   bool skipping = true;
   for (int i = 0, s = 0; i < 4; i++, s += 8)
   {
      int dig = (mVersion >> s) & 0xff;
      if (dig != 0 || !skipping)
      {
         version += !skipping ? wxT(".") : wxT("");
         version += wxString::Format(wxT("%d"), dig);
         skipping = false;
      }
   }

   return version;
}

TranslatableString VSTEffect::GetDescription() const
{
   // VST does have a product string opcode and some effects return a short
   // description, but most do not or they just return the name again.  So,
   // try to provide some sort of useful information.
   return XO("Audio In: %d, Audio Out: %d").Format( mAudioIns, mAudioOuts );
}

// ============================================================================
// EffectDefinitionInterface Implementation
// ============================================================================

EffectType VSTEffect::GetType() const
{
   if (mAudioIns == 0 && mAudioOuts == 0)
   {
      return EffectTypeTool;
   }

   if (mAudioIns == 0)
   {
      return EffectTypeGenerate;
   }

   if (mAudioOuts == 0)
   {
      return EffectTypeAnalyze;
   }

   return EffectTypeProcess;
}


EffectFamilySymbol VSTEffect::GetFamily() const
{
   return VSTPLUGINTYPE;
}

bool VSTEffect::IsInteractive() const
{
   return mInteractive;
}

bool VSTEffect::IsDefault() const
{
   return false;
}

auto VSTEffect::RealtimeSupport() const -> RealtimeSince
{
   return RealtimeSince::Always;

   /* return GetType() == EffectTypeProcess
      ? RealtimeSince::Always
      : RealtimeSince::Never; */
}

bool VSTEffect::SupportsAutomation() const
{
   return mAutomatable;
}

bool VSTEffect::InitializePlugin()
{
   if (!mAEffect)
   {
      Load();
   }

   if (!mAEffect)
   {
      return false;
   }

   return true;
}
#endif

VSTMessage::~VSTMessage() = default;

auto VSTMessage::Clone() const -> std::unique_ptr<Message>
{
   auto result = std::make_unique<VSTMessage>(*this);
   // Make sure of the chunk capacity
   result->mChunk.reserve(this->mChunk.capacity());

   return result;
}

void VSTMessage::Assign(Message && src)
{
   VSTMessage& vstSrc = static_cast<VSTMessage&>(src);

   mChunk = vstSrc.mChunk;
   vstSrc.mChunk.resize(0);     // capacity will be preserved though

   assert(mParamsVec.size() == vstSrc.mParamsVec.size());

   for (size_t i = 0; i < mParamsVec.size(); i++)
   {
      mParamsVec[i] = vstSrc.mParamsVec[i];

      // consume the source value
      vstSrc.mParamsVec[i] = std::nullopt;
   }
}

void VSTMessage::Merge(Message && src)
{
   VSTMessage& vstSrc = static_cast<VSTMessage&>(src);

   bool chunkWasAssigned = false;

   if ( ! vstSrc.mChunk.empty() )
   {
      mChunk = vstSrc.mChunk;
      chunkWasAssigned = true;
   }

   vstSrc.mChunk.resize(0);  // capacity will be preserved though

   assert(mParamsVec.size() == vstSrc.mParamsVec.size());

   for (size_t i = 0; i < mParamsVec.size(); i++)
   {
      if (chunkWasAssigned)
      {
         mParamsVec[i] = vstSrc.mParamsVec[i];
      }
      else
      {
         // if src val is nullopt, do not copy it to dest
         if (vstSrc.mParamsVec[i] != std::nullopt)
         {
            mParamsVec[i] = vstSrc.mParamsVec[i];
         }
      }

      // consume the source value
      vstSrc.mParamsVec[i] = std::nullopt;
   }

}

#if 0
std::unique_ptr<EffectInstance::Message> VSTInstance::MakeMessage() const
{
   // The purpose here is just to allocate vectors (chunk and paramVector)
   // with sufficient size, not to get the values too
   VSTSettings settings;
   FetchSettings(settings, /* doFetch = */ false);

   VSTMessage::ParamVector paramVector;
   paramVector.resize(mAEffect->numParams, std::nullopt);

   return std::make_unique<VSTMessage>( std::move(settings.mChunk), std::move(paramVector) );
}


std::unique_ptr<EffectInstance::Message> VSTInstance::MakeMessage(int id, double value) const
{
   return std::make_unique<VSTMessage>(id, value, mAEffect->numParams);
}


std::shared_ptr<EffectInstance> VSTEffect::MakeInstance() const
{
   return const_cast<VSTEffect*>(this)->DoMakeInstance();
}

std::shared_ptr<EffectInstance> VSTEffect::DoMakeInstance()
{
   int userBlockSize;
   GetConfig(*this, PluginSettings::Shared, wxT("Options"),
      wxT("BufferSize"), userBlockSize, 8192);
   size_t userBlockSizeC = std::max( 1, userBlockSize );
   bool useLatency;
   GetConfig(*this, PluginSettings::Shared, wxT("Options"),
      wxT("UseLatency"), useLatency, true);


   return std::make_shared<VSTInstance>(
      *this, mPath, userBlockSizeC, userBlockSizeC, useLatency);
}

unsigned VSTInstance::GetAudioInCount() const
{
   return mAudioIns;
}

unsigned VSTInstance::GetAudioOutCount() const
{
   return mAudioOuts;
}

size_t VSTInstance::SetBlockSize(size_t maxBlockSize)
{
   // Issue 3935 for IEM plug-ins, VST 2 versions:
   // It is mysterious why this further limitation of size works to
   // prevent the crashes in destructive processing, or why this is not
   // needed for non-destructive, but here it is
   // Those plugins report many channels (like 64) but most others will not
   // be affected by these lines with the default size of 8192
   // Note it may make the Block Size option of the settings dialog misleading
   auto numChannels = std::max({ 1u, GetAudioInCount(), GetAudioOutCount() });
   maxBlockSize = std::max(size_t(1),
      std::min(maxBlockSize, size_t(0x8000u / numChannels)));

   mBlockSize = std::min( maxBlockSize, mUserBlockSize );
   return mBlockSize;
}

size_t VSTInstance::GetBlockSize() const
{
   return mBlockSize;
}

auto VSTInstance::GetLatency(
   const EffectSettings& settings, double sampleRate) const -> SampleCount
{
   if (mUseLatency)
      return mBufferDelay;
   return 0;
}

bool VSTInstance::IsReady()
{
   return mReady;
}

bool VSTInstance::ProcessInitialize(
   EffectSettings& settings, double sampleRate, ChannelNames)
{
   // Issue 3942: Copy the contents of settings first.
   // settings may refer to what is in the RealtimeEffectState, but that might
   // get reassigned by EffectSettingsAccess::Set, when the validator's
   // Automate() is called-back by the plug-in during callSetParameter.
   // So this avoids a dangling reference.
   auto copiedSettings = GetSettings(settings);
   StoreSettings(copiedSettings);

   return DoProcessInitialize(sampleRate);
}

bool VSTInstance::DoProcessInitialize(double sampleRate)
{
   // Initialize time info
   memset(&mTimeInfo, 0, sizeof(mTimeInfo));
   mTimeInfo.sampleRate = sampleRate;
   mTimeInfo.nanoSeconds = wxGetUTCTimeMillis().ToDouble();
   mTimeInfo.tempo = 120.0;
   mTimeInfo.timeSigNumerator = 4;
   mTimeInfo.timeSigDenominator = 4;
   mTimeInfo.flags = kVstTempoValid | kVstNanosValid | kVstTransportPlaying;

   // Set processing parameters...power must be off for this
   callDispatcher(effSetSampleRate, 0, 0, NULL, sampleRate);
   callDispatcher(effSetBlockSize, 0, mBlockSize, NULL, 0.0);

   // Turn on the power
   PowerOn();

   // Set the initial buffer delay
   SetBufferDelay(mAEffect->initialDelay);

   mReady = true;
   return true;
}


bool VSTInstance::ProcessFinalize() noexcept
{
   return GuardedCall<bool>([&] {
      mReady = false;

      PowerOff();

      return true;
   });

}


size_t VSTInstance::ProcessBlock(EffectSettings &,
   const float *const *inBlock, float *const *outBlock, size_t blockLen)
{
   // Only call the effect if there's something to do...some do not like zero-length block
   if (blockLen)
   {
      // Go let the plugin moleste the samples
      callProcessReplacing(inBlock, outBlock, blockLen);

      // And track the position
      mTimeInfo.samplePos += (double) blockLen;
   }

   return blockLen;
}


bool VSTInstance::RealtimeInitialize(EffectSettings &settings, double sampleRate)
{
   // Temporarily disconnect from any validator, so that setting the chunk
   // does not cause Automate() callbacks (as some effects will do) that then
   // would send slider movement messages that might destroy information in
   // the settings.
   auto vr = valueRestorer(mpOwningValidator, (VSTUIWrapper*)nullptr);
   return ProcessInitialize(settings, sampleRate, {});
}

bool VSTInstance::RealtimeAddProcessor(EffectSettings &settings,
   EffectOutputs *, unsigned numChannels, float sampleRate)
{
   if (!mRecruited)
   {
      // Assign self to the first processor
      mRecruited = true;
      return true;
   }

   auto &effect = static_cast<const PerTrackEffect &>(mProcessor);
   auto slave = std::make_unique<VSTInstance>(
      const_cast<PerTrackEffect &>(effect),
      mPath, mBlockSize, mUserBlockSize, mUseLatency);

   slave->SetBlockSize(mBlockSize);

   if (!slave->ProcessInitialize(settings, sampleRate, ChannelNames()))
      return false;

   mSlaves.emplace_back(move(slave));
   return true;
}

bool VSTInstance::RealtimeFinalize(EffectSettings&) noexcept
{
return GuardedCall<bool>([&]{

   if (mpOwningValidator)
      mpOwningValidator->Flush();

   mRecruited = false;

   for (const auto &slave : mSlaves)
      slave->ProcessFinalize();
   mSlaves.clear();

   return ProcessFinalize();
});
}

bool VSTInstance::RealtimeSuspend()
{
   PowerOff();

   for (const auto &slave : mSlaves)
      slave->PowerOff();

   return true;
}

bool VSTInstance::RealtimeResume()
{
   PowerOn();

   for (const auto &slave : mSlaves)
      slave->PowerOn();

   return true;
}


bool VSTInstance::OnePresetWasLoadedWhilePlaying()
{
   return mPresetLoadedWhilePlaying.exchange(false);
}

void VSTInstance::DeferChunkApplication()
{
   std::lock_guard<std::mutex> guard(mDeferredChunkMutex);

   if (! mChunkToSetAtIdleTime.empty() )
   {
      ApplyChunk(mChunkToSetAtIdleTime);
      mChunkToSetAtIdleTime.resize(0);
   }
}


void VSTInstance::ApplyChunk(std::vector<char>& chunk)
{
   VstPatchChunkInfo info = {
      1, mAEffect->uniqueID, mAEffect->version, mAEffect->numParams, "" };

   const auto len = chunk.size();
   const auto data = chunk.data();

   callSetChunk(true, len, data, &info);
   for (auto& slave : mSlaves)
      slave->callSetChunk(true, len, data, &info);
}


bool VSTInstance::ChunkMustBeAppliedInMainThread() const
{
   // Some plugins (e.g. Melda) can not have their chunk set in the
   // audio thread, resulting in making the whole app hang.
   // This is why we defer the setting of the chunk in the main thread.

   const bool IsAudioThread = (mMainThreadId != std::this_thread::get_id());

   return IsAudioThread && mIsMeldaPlugin;
}


bool VSTInstance::UsesMessages() const noexcept
{
   return true;
}

bool VSTInstance::RealtimeProcessStart(MessagePackage& package)
{
   const bool applyChunkInMainThread = ChunkMustBeAppliedInMainThread();

   if (applyChunkInMainThread)
      mDeferredChunkMutex.lock();

   if (!package.pMessage)
      return true;

   auto& message = static_cast<VSTMessage&>(*package.pMessage);

   auto &chunk = message.mChunk;

   if (!chunk.empty())
   {
      if (applyChunkInMainThread)
      {
         // Apply the chunk later
         //
         mChunkToSetAtIdleTime = chunk;
      }
      else
      {
         // Apply the chunk now
         ApplyChunk(chunk);
      }

      // Don't apply the chunk again until another message supplies a chunk
      chunk.resize(0);

      // Don't return yet.  Maybe some slider movements also accumulated after
      // the change of the chunk.

      const bool IsAudioThread = (mMainThreadId != std::this_thread::get_id());
      if (IsAudioThread)
      {
         // At the moment, the only reason why this method would be called in the audio thread,
         // is because a preset was loaded while playing

         mPresetLoadedWhilePlaying.store(true);
      }

   }


   assert(message.mParamsVec.size() == mAEffect->numParams);

   for (size_t paramID=0; paramID < mAEffect->numParams; paramID++)
   {
      if (message.mParamsVec[paramID])
      {
         float val = (float)(*message.mParamsVec[paramID]);

         // set the change on the recruited "this" instance
         callSetParameter(paramID, val);

         // set the change on any existing slaves
         for (auto& slave : mSlaves)
         {
            slave->callSetParameter(paramID, val);
         }

         // clear the used info
         message.mParamsVec[paramID] = std::nullopt;
      }
   }

   return true;
}

size_t VSTInstance::RealtimeProcess(size_t group, EffectSettings &settings,
   const float *const *inbuf, float *const *outbuf, size_t numSamples)
{
   if (!mRecruited)
   {
      // unexpected!
      return 0;
   }

   wxASSERT(numSamples <= mBlockSize);

   if (group == 0)
   {
      // use the recruited "this" instance
      return ProcessBlock(settings, inbuf, outbuf, numSamples);
   }
   else if (group <= mSlaves.size())
   {
      // use the slave which maps to the group
      return mSlaves[group - 1]->ProcessBlock(settings, inbuf, outbuf, numSamples);
   }
   else
      return 0;
}

bool VSTInstance::RealtimeProcessEnd(EffectSettings &) noexcept
{
   if ( ChunkMustBeAppliedInMainThread() )
      mDeferredChunkMutex.unlock();

   return true;
}

///
/// Some history...
///
/// Before we ran into the Antress plugin problem with buffer size limitations,
/// (see below) we just had a plain old effect loop...get the input samples, pass
/// them to the effect, save the output samples.
///
/// But, the hack I put in to limit the buffer size to only 8k (normally 512k or so)
/// severely impacted performance.  So, Michael C. added some intermediate buffering
/// that sped things up quite a bit and this is how things have worked for quite a
/// while.  It still didn't get the performance back to the pre-hack stage, but it
/// was a definite benefit.
///
/// History over...
///
/// I've recently (May 2014) tried newer versions of the Antress effects and they
/// no longer seem to have a problem with buffer size.  So, I've made a bit of a
/// compromise...I've made the buffer size user configurable.  Should have done this
/// from the beginning.  I've left the default 8k, just in case, but now the user
/// can set the buffering based on their specific setup and needs.
///
/// And at the same time I added buffer delay compensation, which allows Audacity
/// to account for latency introduced by some effects.  This is based on information
/// provided by the effect, so it will not work with all effects since they don't
/// all provide the information (kn0ck0ut is one).
///
int VSTEffect::ShowClientInterface(const EffectPlugin &,
   wxWindow &parent, wxDialog &dialog,
   EffectEditor* pEditor, bool forceModal) const
{
   //   mProcessLevel = 1;      // in GUI thread

   VSTEditor* vstEditor = static_cast<VSTEditor*>(pEditor);

   return vstEditor->ShowDialog(/* nonModal = */ SupportsRealtime() && !forceModal);
}

int VSTEditor::ShowDialog(bool nonModal)
{
   mDialog->CentreOnParent();

   if (nonModal)
   {
      mDialog->Show();
      return 0;
   }

   return mDialog->ShowModal();
}

bool VSTEditor::IsGraphicalUI()
{
   return mGui;
}

bool VSTEffect::SaveSettings(const EffectSettings& settings, CommandParameters& parms) const
{
   const VSTSettings& vstSettings = GetSettings(settings);

   for (const auto& item : vstSettings.mParamsMap)
   {
      if (item.second)
      {
         const auto& name  =   item.first;
         const auto& value = *(item.second);

         if (!parms.Write(name, value))
         {
            return false;
         }
      }
   }

   return true;
}


bool VSTEffect::LoadSettings(const CommandParameters& parms, EffectSettings& settings) const
{
   VSTSettings& vstSettings = GetSettings(settings);

   long index{};
   wxString key;
   double value = 0.0;
   if (parms.GetFirstEntry(key, index))
   {
      do
      {
         if (parms.Read(key, &value)) {
            auto &map = vstSettings.mParamsMap;
            auto iter = map.find(key);
            if (iter != map.end()) {
               if (iter->second)
                  // Should be guaranteed by MakeSettings
                  iter->second = value;
               else {
                  assert(false);
               }
            }
            else
               // Unknown parameter name in the file
               return false;
         }
      } while (parms.GetNextEntry(key, index));
   }

   // Loads key-value pairs only from a config file -- no chunk
   vstSettings.mChunk.resize(0);
   vstSettings.mVersion   = VSTWrapper::mVersion;
   vstSettings.mUniqueID  = VSTWrapper::mAEffect->uniqueID;
   vstSettings.mNumParams = VSTWrapper::mAEffect->numParams;

   return true;
}

RegistryPaths VSTEffect::GetFactoryPresets() const
{
   RegistryPaths progs;

   // Some plugins, like Guitar Rig 5, only report 128 programs while they have hundreds.  While
   // I was able to come up with a hack in the Guitar Rig case to gather all of the program names
   // it would not let me set a program outside of the first 128.
   if (mVstVersion >= 2)
   {
      for (int i = 0; i < mAEffect->numPrograms; i++)
      {
         progs.push_back(GetString(effGetProgramNameIndexed, i));
      }
   }

   return progs;
}

OptionalMessage
VSTEffect::LoadFactoryPreset(int id, EffectSettings& settings) const
{
   // To do: externalize state so const_cast isn't needed
   bool loadOK = const_cast<VSTEffect*>(this)->DoLoadFactoryPreset(id) &&
      FetchSettings(GetSettings(settings));
   if (!loadOK)
      return {};
   return MakeMessageFS(
      VSTInstance::GetSettings(settings));
}

bool VSTEffect::DoLoadFactoryPreset(int id)
{
   callSetProgram(id);

   return true;
}

std::unique_ptr<EffectEditor> VSTEffect::PopulateUI(const EffectPlugin &,
   ShuttleGui &S, EffectInstance& instance, EffectSettingsAccess &access,
   const EffectOutputs *) const
{
   auto parent = S.GetParent();

   // Determine whether fancy UI is available
   bool gui = mGui;

   // Then use fancy UI only if preferences say so
   if (gui)
      GetConfig(*this, PluginSettings::Shared, wxT("Options"),
                             wxT("UseGUI"),
                             gui,
                          true);

   auto pParent = S.GetParent();

   auto& vst2Instance = dynamic_cast<VSTInstance&>(instance);

   auto editor = std::make_unique<VSTEditor>(
      vst2Instance, gui, *this, access, pParent, mAEffect->numParams);

   // Also let the instance know about the validator, so it can forward
   // to it calls coming from the vst callback
   vst2Instance.SetOwningValidator(editor.get());


   // Build the appropriate dialog type
   if (mGui)
   {
      editor->BuildFancy(instance);
   }
   else
   {
      editor->BuildPlain(access, GetType(), mProjectRate);
   }


   return editor;
}

std::unique_ptr<EffectEditor> VSTEffect::MakeEditor(
   ShuttleGui &, EffectInstance &, EffectSettingsAccess &,
   const EffectOutputs *) const
{
   //! Will not come here because Effect::PopulateUI is overridden
   assert(false);
   return nullptr;
}

bool VSTEffect::CanExportPresets() const
{
   return true;
}

// Throws exceptions rather than reporting errors.
void VSTEffect::ExportPresets(
   const EffectPlugin &, const EffectSettings& settings) const
{
   wxString path;

   // Ask the user for the real name
   //
   // Passing a valid parent will cause some effects dialogs to malfunction
   // upon returning from the SelectFile().
   path = SelectFile(FileNames::Operation::Presets,
      XO("Save VST Preset As:"),
      wxEmptyString,
      wxT("preset"),
      wxT("xml"),
      {
        { XO("Standard VST bank file"), { wxT("fxb") }, true },
        { XO("Standard VST program file"), { wxT("fxp") }, true },
        { XO("Audacity VST preset file"), { wxT("xml") }, true },
      },
      wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxRESIZE_BORDER,
      NULL);

   // User canceled...
   if (path.empty())
   {
      return;
   }

   if ( ! StoreSettings(GetSettings(settings)) )
      return;

   wxFileName fn(path);
   wxString ext = fn.GetExt();
   if (ext.CmpNoCase(wxT("fxb")) == 0)
   {
      SaveFXB(fn);
   }
   else if (ext.CmpNoCase(wxT("fxp")) == 0)
   {
      SaveFXP(fn);
   }
   else if (ext.CmpNoCase(wxT("xml")) == 0)
   {
      // may throw
      SaveXML(fn);
   }
   else
   {
      // This shouldn't happen, but complain anyway
      AudacityMessageBox(
         XO("Unrecognized file extension."),
         XO("Error Saving VST Presets"),
         wxOK | wxCENTRE,
         nullptr);

      return;
   }
}

//
// Load an "fxb", "fxp" or Audacuty "xml" file
//
// Based on work by Sven Giermann
//
OptionalMessage VSTEffect::ImportPresets(const EffectPlugin&,
   EffectSettings& settings) const
{
   auto temp = std::make_unique<VSTEffect>(this->mPath);
   if (!temp->InitializePlugin())
      return {};
   return temp->ImportPresetsNC(settings);
}

OptionalMessage VSTEffect::ImportPresetsNC(EffectSettings& settings)
{
   wxString path;

   // Ask the user for the real name
   path = SelectFile(FileNames::Operation::Presets,
      XO("Load VST Preset:"),
      wxEmptyString,
      wxT("preset"),
      wxT("xml"),
      { {
         XO("VST preset files"),
         { wxT("fxb"), wxT("fxp"), wxT("xml") },
         true
      } },
      wxFD_OPEN | wxRESIZE_BORDER,
      nullptr);

   // User canceled...
   if (path.empty())
   {
      return {};
   }

   wxFileName fn(path);
   wxString ext = fn.GetExt();
   bool success = false;
   if (ext.CmpNoCase(wxT("fxb")) == 0)
   {
      success = LoadFXB(fn);
   }
   else if (ext.CmpNoCase(wxT("fxp")) == 0)
   {
      success = LoadFXP(fn);
   }
   else if (ext.CmpNoCase(wxT("xml")) == 0)
   {
      success = LoadXML(fn);
   }
   else
   {
      // This shouldn't happen, but complain anyway
      AudacityMessageBox(
         XO("Unrecognized file extension."),
         XO("Error Loading VST Presets"),
         wxOK | wxCENTRE,
         nullptr);

      return {};
   }

   if (!success)
   {
      AudacityMessageBox(
         XO("Unable to load presets file."),
         XO("Error Loading VST Presets"),
         wxOK | wxCENTRE,
         nullptr);

      return {};
   }

   if (!FetchSettings(GetSettings(settings)))
      return {};

   return MakeMessageFS(
      VSTInstance::GetSettings(settings));
}

bool VSTEffect::HasOptions() const
{
   return true;
}

void VSTEffect::ShowOptions(const EffectPlugin &) const
{
   VSTEffectOptionsDialog{ *this }.ShowModal();
}

// ============================================================================
// VSTEffect implementation
// ============================================================================

#endif

bool VSTWrapper::Load()
{
   vstPluginMain pluginMain;
   bool success = false;

   long effectID = 0;
   wxString realPath = mPath.BeforeFirst(wxT(';'));
   mPath.AfterFirst(wxT(';')).ToLong(&effectID);
   mCurrentEffectID = (intptr_t) effectID;

   mModule = NULL;
   mAEffect = NULL;

#if defined(__WXMAC__)
   // Start clean
   mBundleRef.reset();

   mResource = ResourceHandle{};

   // Convert the path to a CFSTring
   wxCFStringRef path(realPath);

   // Create the bundle using the URL
   BundleHandle bundleRef{ CFBundleCreate(kCFAllocatorDefault,
      // Convert the path to a URL
      CF_ptr<CFURLRef>{
         CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
            path, kCFURLPOSIXPathStyle, true)
      }.get()
   )};

   // Bail if the bundle wasn't created
   if (!bundleRef)
      return false;


   // Convert back to path
   UInt8 exePath[PLATFORM_MAX_PATH];
   Boolean good = CFURLGetFileSystemRepresentation(
      // Retrieve a reference to the executable
      CF_ptr<CFURLRef>{ CFBundleCopyExecutableURL(bundleRef.get()) }.get(),
      true, exePath, sizeof(exePath)
   );

   // Bail if we couldn't resolve the executable path
   if (good == FALSE)
      return false;

   // Attempt to open it
   mModule.reset((char*)dlopen((char *) exePath, RTLD_NOW | RTLD_LOCAL));
   if (!mModule)
      return false;

   // Try to locate the NEW plugin entry point
   pluginMain = (vstPluginMain) dlsym(mModule.get(), "VSTPluginMain");

   // If not found, try finding the old entry point
   if (pluginMain == NULL)
   {
      pluginMain = (vstPluginMain) dlsym(mModule.get(), "main_macho");
   }

   // Must not be a VST plugin
   if (pluginMain == NULL)
   {
      mModule.reset();
      return false;
   }

   // Need to keep the bundle reference around so we can map the
   // resources.
   mBundleRef = std::move(bundleRef);

   // Open the resource map ... some plugins (like GRM Tools) need this.
   mResource = ResourceHandle{
      mBundleRef.get(), CFBundleOpenBundleResourceMap(mBundleRef.get())
   };

#elif defined(__WXMSW__)

   {
      wxLogNull nolog;

      // Try to load the library
      auto lib = std::make_unique<wxDynamicLibrary>(realPath);
      if (!lib)
         return false;

      // Bail if it wasn't successful
      if (!lib->IsLoaded())
         return false;

      // Try to find the entry point, while suppressing error messages
      pluginMain = (vstPluginMain) lib->GetSymbol(wxT("VSTPluginMain"));
      if (pluginMain == NULL)
      {
         pluginMain = (vstPluginMain) lib->GetSymbol(wxT("main"));
         if (pluginMain == NULL)
            return false;
      }

      // Save the library reference
      mModule = std::move(lib);
   }

#else

   // Attempt to load it
   //
   // Spent a few days trying to figure out why some VSTs where running okay and
   // others were hit or miss.  The cause was that we export all of Audacity's
   // symbols and some of the loaded libraries were picking up Audacity's and
   // not their own.
   //
   // So far, I've only seen this issue on Linux, but we might just be getting
   // lucky on the Mac and Windows.  The sooner we stop exporting everything
   // the better.
   //
   // To get around the problem, I just added the RTLD_DEEPBIND flag to the load
   // and that "basically" puts Audacity last when the loader needs to resolve
   // symbols.
   //
   // Once we define a proper external API, the flags can be removed.
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif
   ModuleHandle lib {
      (char*) dlopen((const char *)wxString(realPath).ToUTF8(),
                     RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND)
   };
   if (!lib)
   {
      return false;
   }

   // Try to find the entry point, while suppressing error messages
   pluginMain = (vstPluginMain) dlsym(lib.get(), "VSTPluginMain");
   if (pluginMain == NULL)
   {
      pluginMain = (vstPluginMain) dlsym(lib.get(), "main");
      if (pluginMain == NULL)
         return false;
   }

   // Save the library reference
   mModule = std::move(lib);

#endif

   // Initialize the plugin
   try
   {
      mAEffect = pluginMain(VSTWrapper::AudioMaster);
   }
   catch (...)
   {
      wxLogMessage(wxT("VST plugin initialization failed\n"));
      mAEffect = NULL;
   }

   // Was it successful?
   if (mAEffect)
   {
      mGui = (mAEffect->flags & effFlagsHasEditor);

      // Must use the GUI editor if parameters aren't provided
      if (mAEffect->numParams == 0)
      {
         mGui = true;
      }

      // Save a reference to ourselves
      //
      // Note:  Some hosts use "user" and some use "ptr2/resvd2".  It might
      //        be worthwhile to check if user is NULL before using it and
      //        then falling back to "ptr2/resvd2".
      mAEffect->ptr2 = static_cast<VSTWrapper*>(this);

      // Give the plugin an initial sample rate and blocksize
      callDispatcher(effSetSampleRate, 0, 0, NULL, 48000.0);
      callDispatcher(effSetBlockSize, 0, 512, NULL, 0);

      // Ask the plugin to identify itself...might be needed for older plugins
      callDispatcher(effIdentify, 0, 0, NULL, 0);

      // Open the plugin
      callDispatcher(effOpen, 0, 0, NULL, 0.0);

      // Get the VST version the plugin understands
      mVstVersion = callDispatcher(effGetVstVersion, 0, 0, NULL, 0);

      // Set it again in case plugin ignored it before the effOpen
      callDispatcher(effSetSampleRate, 0, 0, NULL, 48000.0);
      callDispatcher(effSetBlockSize, 0, 512, NULL, 0);

      // Ensure that it looks like a plugin and can deal with ProcessReplacing
      // calls.  Also exclude synths for now.
      if (mAEffect->magic == kEffectMagic &&
         !(mAEffect->flags & effFlagsIsSynth) &&
         mAEffect->flags & effFlagsCanReplacing)
      {
         if (mVstVersion >= 2)
         {
            mName = GetString(effGetEffectName);
            if (mName.length() == 0)
            {
               mName = GetString(effGetProductString);
            }
         }
         if (mName.length() == 0)
         {
            mName = wxFileName{realPath}.GetName();
         }

         if (mVstVersion >= 2)
         {
            mVendor = GetString(effGetVendorString);
            mVersion = wxINT32_SWAP_ON_LE(callDispatcher(effGetVendorVersion, 0, 0, NULL, 0));
         }
         if (mVersion == 0)
         {
            mVersion = wxINT32_SWAP_ON_LE(mAEffect->version);
         }

         if (mAEffect->flags & effFlagsHasEditor || mAEffect->numParams != 0)
         {
            mInteractive = true;
         }

         mAudioIns = mAEffect->numInputs;
         mAudioOuts = mAEffect->numOutputs;

         // Check to see if parameters can be automated.  This isn't a guarantee
         // since it could be that the effect simply doesn't support the opcode.
         mAutomatable = false;
         for (int i = 0; i < mAEffect->numParams; i++)
         {
            if (callDispatcher(effCanBeAutomated, 0, i, NULL, 0.0))
            {
               mAutomatable = true;
               break;
            }
         }

         // Make sure we start out with a valid program selection
         // I've found one plugin (SoundHack +morphfilter) that will
         // crash Audacity when saving the initial default parameters
         // with this.
         callSetProgram(0);

         // Pretty confident that we're good to go
         success = true;
      }
   }

   if (!success)
   {
      Unload();
      ResetModuleAndHandle();
   }

   return success;
}

void VSTWrapper::Unload()
{
   if (mAEffect)
   {
      // Finally, close the plugin
      callDispatcher(effClose, 0, 0, NULL, 0.0);
      mAEffect = NULL;
   }

   //ResetModuleAndHandle();
}

void VSTWrapper::ResetModuleAndHandle()
{
   if (mModule)
   {
#if defined(__WXMAC__)
      mResource.reset();
      mBundleRef.reset();
#endif

      mModule.reset();
      mAEffect = NULL;
   }
}

VSTWrapper::~VSTWrapper()
{
   Unload();
   ResetModuleAndHandle();
}

VstPatchChunkInfo VSTWrapper::GetChunkInfo() const
{
   VstPatchChunkInfo info = { 1, mAEffect->uniqueID, mAEffect->version, mAEffect->numParams, "" };
   return info;
}

bool VSTWrapper::IsCompatible(const VstPatchChunkInfo& info) const
{
   return  (info.pluginUniqueID == mAEffect->uniqueID) &&
           (info.pluginVersion  == mAEffect->version) &&
           (info.numElements    == mAEffect->numParams);
}

#if 0
OptionalMessage VSTEffect::LoadUserPreset(
   const RegistryPath & group, EffectSettings &settings) const
{
   wxString value;

   auto info = GetChunkInfo();

   GetConfig(*this, PluginSettings::Private, group, wxT("UniqueID"),
      info.pluginUniqueID, info.pluginUniqueID);
   GetConfig(*this, PluginSettings::Private, group, wxT("Version"),
      info.pluginVersion, info.pluginVersion);
   GetConfig(*this, PluginSettings::Private, group, wxT("Elements"),
      info.numElements, info.numElements);

   if ( ! IsCompatible(info) )
   {
      return {};
   }

   if (GetConfig(*this,
      PluginSettings::Private, group, wxT("Chunk"), value, wxEmptyString))
   {
      ArrayOf<char> buf{ value.length() / 4 * 3 };

      int len = Base64::Decode(value, buf.get());
      if (len)
      {
         callSetChunk(true, len, buf.get(), &info);
         if (!FetchSettings(GetSettings(settings)))
            return {};
      }

      return MakeMessageFS(
         VSTInstance::GetSettings(settings));
   }

   wxString parms;
   if (!GetConfig(*this,
      PluginSettings::Private, group, wxT("Parameters"), parms, wxEmptyString))
   {
      return {};
   }

   CommandParameters eap;
   if (!eap.SetParameters(parms))
   {
      return {};
   }

   const bool loadOK = LoadSettings(eap, settings) &&
      FetchSettings(GetSettings(settings));
   if (!loadOK)
      return {};

   return MakeMessageFS(
      VSTInstance::GetSettings(settings));
}


bool VSTEffect::SaveUserPreset(
   const RegistryPath & group, const EffectSettings &settings) const
{
   const auto& vstSettings = GetSettings(settings);

   if ( ! StoreSettings(vstSettings) )
      return false;

   SetConfig(*this, PluginSettings::Private, group, wxT("UniqueID"), vstSettings.mUniqueID );
   SetConfig(*this, PluginSettings::Private, group, wxT("Version"),  vstSettings.mVersion  );
   SetConfig(*this, PluginSettings::Private, group, wxT("Elements"), vstSettings.mNumParams);

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      void *chunk = NULL;
      int clen = (int) constCallDispatcher(effGetChunk, 1, 0, &chunk, 0.0);
      if (clen <= 0)
      {
         return false;
      }

      SetConfig(*this, PluginSettings::Private, group, wxT("Chunk"),
         Base64::Encode(chunk, clen));
      return true;
   }

   CommandParameters eap;
   if (!SaveSettings(settings, eap))
   {
      return false;
   }

   wxString parms;
   if (!eap.GetParameters(parms))
   {
      return false;
   }

   return SetConfig(*this, PluginSettings::Private,
      group, wxT("Parameters"), parms);
}

#endif

void VSTUIWrapper::Flush()
{}

#if 0
void VSTEditor::Flush()
{
   mAccess.Flush();
}

void VSTEditor::OnTimer()
{
   wxRecursionGuard guard(mTimerGuard);

   // Ignore it if we're recursing
   if (guard.IsInside())
   {
      return;
   }

   if (GetInstance().mVstVersion >= 2 && mWantsIdle)
   {
      int ret = GetInstance().callDispatcher(effIdle, 0, 0, NULL, 0.0);
      if (!ret)
      {
         mWantsIdle = false;
      }
   }

   if (mWantsEditIdle)
   {
      GetInstance().callDispatcher(effEditIdle, 0, 0, NULL, 0.0);
   }
}

#endif

void VSTUIWrapper::NeedIdle()
{
}

VstTimeInfo* VSTWrapper::GetTimeInfo()
{
   mTimeInfo.nanoSeconds = wxGetUTCTimeMillis().ToDouble();
   return &mTimeInfo;
}

float VSTWrapper::GetSampleRate()
{
   return mTimeInfo.sampleRate;
}

int VSTWrapper::GetProcessLevel()
{
   return mProcessLevel;
}

#if 0
void VSTInstance::PowerOn()
{
   if (!mHasPower)
   {
      // Turn the power on
      callDispatcher(effMainsChanged, 0, 1, NULL, 0.0);

      // Tell the effect we're going to start processing
      if (mVstVersion >= 2)
      {
         callDispatcher(effStartProcess, 0, 0, NULL, 0.0);
      }

      // Set state
      mHasPower = true;
   }
}

void VSTInstance::PowerOff()
{
   if (mHasPower)
   {
      // Tell the effect we're going to stop processing
      if (mVstVersion >= 2)
      {
         callDispatcher(effStopProcess, 0, 0, NULL, 0.0);
      }

      // Turn the power off
      callDispatcher(effMainsChanged, 0, 0, NULL, 0.0);

      // Set state
      mHasPower = false;
   }
}

#endif

void VSTUIWrapper::SizeWindow(int w, int h)
{
}

#if 0
void VSTInstance::SizeWindow(int w, int h)
{
   if (mpOwningValidator)
   {
      mpOwningValidator->SizeWindow(w, h);
   }
}

void VSTEditor::NotifyParameterChanged(int index, float value)
{
   const auto& settings = VSTWrapper::GetSettings(mAccess.Get());

   GetInstance().ForEachParameter(
      [index, value, &settings, this](const auto& pi)
      {
         if (pi.mID != index)
            return true;

         auto it = settings.mParamsMap.find(pi.mName);

         // For consistency with other plugin families
         constexpr float epsilon = 1.0e-5f;

         if (
            it == settings.mParamsMap.end() || !it->second.has_value() ||
            std::abs(*it->second - value) > epsilon)
            Publish(EffectSettingChanged { size_t(index), value });

         return false;
      });
}

void VSTEditor::OnIdle(wxIdleEvent& evt)
{
   evt.Skip();
   if (!mLastMovements.empty()) {
      // Be sure the instance has got any messages
      mAccess.Flush();
      mAccess.ModifySettings([&](EffectSettings& settings) {
         // Update settings, for stickiness
         // But don't do a complete FetchSettingsFromInstance
         for (auto [index, value] : mLastMovements) {
            if (index >= 0 && index < mParamNames.size()) {
               const auto &string = mParamNames[index];
               auto &mySettings = VSTWrapper::GetSettings(settings);
               mySettings.mParamsMap[string] = value;
            }
         }
         // Succeed but with a null message
         return nullptr;
      });
      for (auto [index, _] : mLastMovements)
         RefreshParameters(index);
      mLastMovements.clear();
   }

   GetInstance().DeferChunkApplication();

   if ( GetInstance().OnePresetWasLoadedWhilePlaying() )
   {
      RefreshParameters();
   }

}

void VSTEditor::SizeWindow(int w, int h)
{
   // Queue the event to make the resizes smoother
   if (mParent)
   {
      wxCommandEvent sw(EVT_SIZEWINDOW);
      sw.SetInt(w);
      sw.SetExtraLong(h);
      mParent->GetEventHandler()->AddPendingEvent(sw);
   }

   return;
}

void VSTEffect::UpdateDisplay()
{
#if 0
   // Tell the dialog to refresh effect information
   if (mParent)
   {
      wxCommandEvent ud(EVT_UPDATEDISPLAY);
      mParent->GetEventHandler()->AddPendingEvent(ud);
   }
#endif
   return;
}


#endif

void VSTWrapper::SetBufferDelay(int)
{
}

#if 0
void VSTInstance::SetBufferDelay(int samples)
{
   // We do not support negative delay
   if (samples >= 0 && mUseLatency)
   {
      mBufferDelay = samples;
   }

   return;
}
#endif

int VSTWrapper::GetString(wxString & outstr, int opcode, int index) const
{
   char buf[256];

   memset(buf, 0, sizeof(buf));

   // Assume we are passed a read-only dispatcher function code
   constCallDispatcher(opcode, index, 0, buf, 0.0);

   outstr = wxString::FromUTF8(buf);

   return 0;
}

wxString VSTWrapper::GetString(int opcode, int index) const
{
   wxString str;

   GetString(str, opcode, index);

   return str;
}

void VSTWrapper::SetString(int opcode, const wxString & str, int index)
{
   char buf[256];
   strcpy(buf, str.Left(255).ToUTF8());

   callDispatcher(opcode, index, 0, buf, 0.0);
}

intptr_t VSTWrapper::callDispatcher(int opcode,
                                   int index, intptr_t value, void *ptr, float opt)
{
   // Needed since we might be in the dispatcher when the timer pops
   std::lock_guard guard(mDispatcherLock);

   return mAEffect->dispatcher(mAEffect, opcode, index, value, ptr, opt);
}

intptr_t VSTWrapper::constCallDispatcher(int opcode,
   int index, intptr_t value, void *ptr, float opt) const
{
   // Assume we are passed a read-only dispatcher function code
   return const_cast<VSTWrapper*>(this)
      ->callDispatcher(opcode, index, value, ptr, opt);
}

#if 0
void VSTInstance::callProcessReplacing(const float *const *inputs,
   float *const *outputs, int sampleframes)
{
   mAEffect->processReplacing(mAEffect,
      const_cast<float**>(inputs),
      const_cast<float**>(outputs), sampleframes);
}
#endif

float VSTWrapper::callGetParameter(int index) const
{
   return mAEffect->getParameter(mAEffect, index);
}

void VSTWrapper::callSetParameter(int index, float value) const
{
   if (mVstVersion == 0 || constCallDispatcher(effCanBeAutomated, 0, index, NULL, 0.0))
   {
      mAEffect->setParameter(mAEffect, index, value);
   }
}

void VSTWrapper::callSetProgram(int index)
{
   callDispatcher(effBeginSetProgram, 0, 0, NULL, 0.0);

   callDispatcher(effSetProgram, 0, index, NULL, 0.0);

   callDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);
}

void VSTWrapper::callSetChunk(bool isPgm, int len, void *buf)
{
   VstPatchChunkInfo info;

   memset(&info, 0, sizeof(info));
   info.version = 1;
   info.pluginUniqueID = mAEffect->uniqueID;
   info.pluginVersion = mAEffect->version;
   info.numElements = isPgm ? mAEffect->numParams : mAEffect->numPrograms;

   callSetChunk(isPgm, len, buf, &info);
}

void VSTWrapper::callSetChunk(bool isPgm, int len, void *buf, VstPatchChunkInfo *info) const
{
   if (isPgm)
   {
      // Ask the effect if this is an acceptable program
      if (constCallDispatcher(effBeginLoadProgram, 0, 0, info, 0.0) == -1)
      {
         return;
      }
   }
   else
   {
      // Ask the effect if this is an acceptable bank
      if (constCallDispatcher(effBeginLoadBank, 0, 0, info, 0.0) == -1)
      {
         return;
      }
   }

   constCallDispatcher(effBeginSetProgram, 0, 0, NULL, 0.0);
   constCallDispatcher(effSetChunk, isPgm ? 1 : 0, len, buf, 0.0);
   constCallDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);
}

bool VSTWrapper::LoadFXB(const wxFileName & fn)
{
   bool ret = false;

   // Try to open the file...will be closed automatically when method returns
   wxFFile f(fn.GetFullPath(), wxT("rb"));
   if (!f.IsOpened())
   {
      return false;
   }

   // Allocate memory for the contents
   ArrayOf<unsigned char> data{ size_t(f.Length()) };
   if (!data)
   {
      AudacityMessageBox(
         XO("Unable to allocate memory when loading presets file."),
         XO("Error Loading VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
      return false;
   }
   unsigned char *bptr = data.get();

   do
   {
      // Read in the whole file
      ssize_t len = f.Read((void *) bptr, f.Length());
      if (f.Error())
      {
         AudacityMessageBox(
            XO("Unable to read presets file."),
            XO("Error Loading VST Presets"),
            wxOK | wxCENTRE,
            nullptr);
         break;
      }

      // Most references to the data are via an "int" array
      int32_t *iptr = (int32_t *) bptr;

      // Verify that we have at least enough for the header
      if (len < 156)
      {
         break;
      }

      // Verify that we probably have an FX file
      if (wxINT32_SWAP_ON_LE(iptr[0]) != CCONST('C', 'c', 'n', 'K'))
      {
         break;
      }

      // Ignore the size...sometimes it's there, other times it's zero

      // Get the version and verify
      int version = wxINT32_SWAP_ON_LE(iptr[3]);
      if (version != 1 && version != 2)
      {
         break;
      }

      VstPatchChunkInfo info =
      {
         1,
         wxINT32_SWAP_ON_LE(iptr[4]),
         wxINT32_SWAP_ON_LE(iptr[5]),
         wxINT32_SWAP_ON_LE(iptr[6]),
         ""
      };

      // Ensure this program looks to belong to the current plugin
      if ((info.pluginUniqueID != mAEffect->uniqueID) &&
          (info.pluginVersion != mAEffect->version) &&
          (info.numElements != mAEffect->numPrograms))
      {
         break;
      }

      // Get the number of programs
      int numProgs = info.numElements;

      // Get the current program index
      int curProg = 0;
      if (version >= 2)
      {
         curProg = wxINT32_SWAP_ON_LE(iptr[7]);
         if (curProg < 0 || curProg >= numProgs)
         {
            break;
         }
      }

      // Is it a bank of programs?
      if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'x', 'B', 'k'))
      {
         // Drop the header
         bptr += 156;
         len -= 156;

         unsigned char *tempPtr = bptr;
         ssize_t tempLen = len;

         // Validate all of the programs
         for (int i = 0; i < numProgs; i++)
         {
            if (!LoadFXProgram(&tempPtr, tempLen, i, true))
            {
               break;
            }
         }

         // Ask the effect if this is an acceptable bank
         if (callDispatcher(effBeginLoadBank, 0, 0, &info, 0.0) == -1)
         {
            return false;
         }

         // Start loading the individual programs
         for (int i = 0; i < numProgs; i++)
         {
            ret = LoadFXProgram(&bptr, len, i, false);
         }
      }
      // Or maybe a bank chunk?
      else if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'B', 'C', 'h'))
      {
         // Can't load programs chunks if the plugin doesn't support it
         if (!(mAEffect->flags & effFlagsProgramChunks))
         {
            break;
         }

         // Verify that we have enough to grab the chunk size
         if (len < 160)
         {
            break;
         }

         // Get the chunk size
         int size = wxINT32_SWAP_ON_LE(iptr[39]);

         // We finally know the full length of the program
         int proglen = 160 + size;

         // Verify that we have enough for the entire program
         if (len < proglen)
         {
            break;
         }

         // Set the entire bank in one shot
         callSetChunk(false, size, &iptr[40], &info);

         // Success
         ret = true;
      }
      // Unrecognizable type
      else
      {
         break;
      }

      // Set the active program
      if (ret && version >= 2)
      {
         callSetProgram(curProg);
      }
   } while (false);

   return ret;
}

bool VSTWrapper::LoadFXP(const wxFileName & fn)
{
   bool ret = false;

   // Try to open the file...will be closed automatically when method returns
   wxFFile f(fn.GetFullPath(), wxT("rb"));
   if (!f.IsOpened())
   {
      return false;
   }

   // Allocate memory for the contents
   ArrayOf<unsigned char> data{ size_t(f.Length()) };
   if (!data)
   {
      AudacityMessageBox(
         XO("Unable to allocate memory when loading presets file."),
         XO("Error Loading VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
      return false;
   }
   unsigned char *bptr = data.get();

   do
   {
      // Read in the whole file
      ssize_t len = f.Read((void *) bptr, f.Length());
      if (f.Error())
      {
         AudacityMessageBox(
            XO("Unable to read presets file."),
            XO("Error Loading VST Presets"),
            wxOK | wxCENTRE,
            nullptr);
         break;
      }

      // Get (or default) currently selected program
      int i = 0; //mProgram->GetCurrentSelection();
      if (i < 0)
      {
         i = 0;   // default to first program
      }

      // Go verify and set the program
      ret = LoadFXProgram(&bptr, len, i, false);
   } while (false);

   return ret;
}

bool VSTWrapper::LoadFXProgram(unsigned char **bptr, ssize_t & len, int index, bool dryrun)
{
   // Most references to the data are via an "int" array
   int32_t *iptr = (int32_t *) *bptr;

   // Verify that we have at least enough for a program without parameters
   if (len < 28)
   {
      return false;
   }

   // Verify that we probably have an FX file
   if (wxINT32_SWAP_ON_LE(iptr[0]) != CCONST('C', 'c', 'n', 'K'))
   {
      return false;
   }

   // Ignore the size...sometimes it's there, other times it's zero

   // Get the version and verify
#if defined(IS_THIS_AN_FXP_ARTIFICAL_LIMITATION)
   int version = wxINT32_SWAP_ON_LE(iptr[3]);
   if (version != 1)
   {
      return false;
   }
#endif

   VstPatchChunkInfo info =
   {
      1,
      wxINT32_SWAP_ON_LE(iptr[4]),
      wxINT32_SWAP_ON_LE(iptr[5]),
      wxINT32_SWAP_ON_LE(iptr[6]),
      ""
   };

   // Ensure this program looks to belong to the current plugin
   if ((info.pluginUniqueID != mAEffect->uniqueID) &&
         (info.pluginVersion != mAEffect->version) &&
         (info.numElements != mAEffect->numParams))
   {
      return false;
   }

   // Get the number of parameters
   int numParams = info.numElements;

   // At this point, we have to have enough to include the program name as well
   if (len < 56)
   {
      return false;
   }

   // Get the program name
   wxString progName(wxString::From8BitData((char *)&iptr[7]));

   // Might be a regular program
   if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'x', 'C', 'k'))
   {
      // We finally know the full length of the program
      int proglen = 56 + (numParams * sizeof(float));

      // Verify that we have enough for all of the parameter values
      if (len < proglen)
      {
         return false;
      }

      // Validate all of the parameter values
      for (int i = 0; i < numParams; i++)
      {
         uint32_t ival = wxUINT32_SWAP_ON_LE(iptr[14 + i]);
         float val = reinterpretAsFloat(ival);
         if (val < 0.0 || val > 1.0)
         {
            return false;
         }
      }

      // They look okay...time to start changing things
      if (!dryrun)
      {
         // Ask the effect if this is an acceptable program
         if (callDispatcher(effBeginLoadProgram, 0, 0, &info, 0.0) == -1)
         {
            return false;
         }

         // Load all of the parameters
         callDispatcher(effBeginSetProgram, 0, 0, NULL, 0.0);
         for (int i = 0; i < numParams; i++)
         {
            wxUint32 val = wxUINT32_SWAP_ON_LE(iptr[14 + i]);
            callSetParameter(i, reinterpretAsFloat(val));
         }
         callDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);
      }

      // Update in case we're loading an "FxBk" format bank file
      *bptr += proglen;
      len -= proglen;
   }
   // Maybe we have a program chunk
   else if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'P', 'C', 'h'))
   {
      // Can't load programs chunks if the plugin doesn't support it
      if (!(mAEffect->flags & effFlagsProgramChunks))
      {
         return false;
      }

      // Verify that we have enough to grab the chunk size
      if (len < 60)
      {
         return false;
      }

      // Get the chunk size
      int size = wxINT32_SWAP_ON_LE(iptr[14]);

      // We finally know the full length of the program
      int proglen = 60 + size;

      // Verify that we have enough for the entire program
      if (len < proglen)
      {
         return false;
      }

      // Set the entire program in one shot
      if (!dryrun)
      {
         callSetChunk(true, size, &iptr[15], &info);
      }

      // Update in case we're loading an "FxBk" format bank file
      *bptr += proglen;
      len -= proglen;
   }
   else
   {
      // Unknown type
      return false;
   }

   if (!dryrun)
   {
      SetString(effSetProgramName, wxString(progName), index);
   }

   return true;
}

bool VSTWrapper::LoadXML(const wxFileName & fn)
{
   mInChunk = false;
   mInSet = false;

   // default to read as XML file
   // Load the program
   XMLFileReader reader;
   bool ok = reader.Parse(this, fn.GetFullPath());

   // Something went wrong with the file, clean up
   if (mInSet)
   {
      callDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);

      mInSet = false;
   }

   if (!ok)
   {
      // Inform user of load failure
      AudacityMessageBox(
         reader.GetErrorStr(),
         XO("Error Loading VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
      return false;
   }

   return true;
}

void VSTWrapper::SaveFXB(const wxFileName & fn) const
{
   // Create/Open the file
   const wxString fullPath{fn.GetFullPath()};
   wxFFile f(fullPath, wxT("wb"));
   if (!f.IsOpened())
   {
      AudacityMessageBox(
         XO("Could not open file: \"%s\"").Format( fullPath ),
         XO("Error Saving VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
      return;
   }

   wxMemoryBuffer buf;
   wxInt32 subType;
   void *chunkPtr = nullptr;
   int chunkSize = 0;
   int dataSize = 148;
   wxInt32 tab[8];
   int curProg = 0 ; //mProgram->GetCurrentSelection();

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      subType = CCONST('F', 'B', 'C', 'h');

      // read-only dispatcher function
      chunkSize = constCallDispatcher(effGetChunk, 0, 0, &chunkPtr, 0.0);
      dataSize += 4 + chunkSize;
   }
   else
   {
      subType = CCONST('F', 'x', 'B', 'k');

      for (int i = 0; i < mAEffect->numPrograms; i++)
      {
         SaveFXProgram(buf, i);
      }

      dataSize += buf.GetDataLen();
   }

   tab[0] = wxINT32_SWAP_ON_LE(CCONST('C', 'c', 'n', 'K'));
   tab[1] = wxINT32_SWAP_ON_LE(dataSize);
   tab[2] = wxINT32_SWAP_ON_LE(subType);
   tab[3] = wxINT32_SWAP_ON_LE(curProg >= 0 ? 2 : 1);
   tab[4] = wxINT32_SWAP_ON_LE(mAEffect->uniqueID);
   tab[5] = wxINT32_SWAP_ON_LE(mAEffect->version);
   tab[6] = wxINT32_SWAP_ON_LE(mAEffect->numPrograms);
   tab[7] = wxINT32_SWAP_ON_LE(curProg >= 0 ? curProg : 0);

   f.Write(tab, sizeof(tab));
   if (!f.Error())
   {
      char padding[124];
      memset(padding, 0, sizeof(padding));
      f.Write(padding, sizeof(padding));

      if (!f.Error())
      {
         if (mAEffect->flags & effFlagsProgramChunks)
         {
            wxInt32 size = wxINT32_SWAP_ON_LE(chunkSize);
            f.Write(&size, sizeof(size));
            f.Write(chunkPtr, chunkSize);
         }
         else
         {
            f.Write(buf.GetData(), buf.GetDataLen());
         }
      }
   }

   if (f.Error())
   {
      AudacityMessageBox(
         XO("Error writing to file: \"%s\"").Format( fullPath ),
         XO("Error Saving VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
   }

   f.Close();

   return;
}

void VSTWrapper::SaveFXP(const wxFileName & fn) const
{
   // Create/Open the file
   const wxString fullPath{ fn.GetFullPath() };
   wxFFile f(fullPath, wxT("wb"));
   if (!f.IsOpened())
   {
      AudacityMessageBox(
         XO("Could not open file: \"%s\"").Format( fullPath ),
         XO("Error Saving VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
      return;
   }

   wxMemoryBuffer buf;

   // read-only dispatcher function
   int ndx = constCallDispatcher(effGetProgram, 0, 0, NULL, 0.0);
   SaveFXProgram(buf, ndx);

   f.Write(buf.GetData(), buf.GetDataLen());
   if (f.Error())
   {
      AudacityMessageBox(
         XO("Error writing to file: \"%s\"").Format( fullPath ),
         XO("Error Saving VST Presets"),
         wxOK | wxCENTRE,
         nullptr);
   }

   f.Close();

   return;
}

void VSTWrapper::SaveFXProgram(wxMemoryBuffer & buf, int index) const
{
   wxInt32 subType;
   void *chunkPtr;
   int chunkSize;
   int dataSize = 48;
   char progName[28];
   wxInt32 tab[7];

   // read-only dispatcher function
   constCallDispatcher(effGetProgramNameIndexed, index, 0, &progName, 0.0);
   progName[27] = '\0';
   chunkSize = strlen(progName);
   memset(&progName[chunkSize], 0, sizeof(progName) - chunkSize);

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      subType = CCONST('F', 'P', 'C', 'h');

      // read-only dispatcher function
      chunkSize = constCallDispatcher(effGetChunk, 1, 0, &chunkPtr, 0.0);
      dataSize += 4 + chunkSize;
   }
   else
   {
      subType = CCONST('F', 'x', 'C', 'k');

      dataSize += (mAEffect->numParams << 2);
   }

   tab[0] = wxINT32_SWAP_ON_LE(CCONST('C', 'c', 'n', 'K'));
   tab[1] = wxINT32_SWAP_ON_LE(dataSize);
   tab[2] = wxINT32_SWAP_ON_LE(subType);
   tab[3] = wxINT32_SWAP_ON_LE(1);
   tab[4] = wxINT32_SWAP_ON_LE(mAEffect->uniqueID);
   tab[5] = wxINT32_SWAP_ON_LE(mAEffect->version);
   tab[6] = wxINT32_SWAP_ON_LE(mAEffect->numParams);

   buf.AppendData(tab, sizeof(tab));
   buf.AppendData(progName, sizeof(progName));

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      wxInt32 size = wxINT32_SWAP_ON_LE(chunkSize);
      buf.AppendData(&size, sizeof(size));
      buf.AppendData(chunkPtr, chunkSize);
   }
   else
   {
      for (int i = 0; i < mAEffect->numParams; i++)
      {
         float val = callGetParameter(i);
         wxUint32 ival = wxUINT32_SWAP_ON_LE(reinterpretAsUint32(val));
         buf.AppendData(&ival, sizeof(ival));
      }
   }

   return;
}

// Throws exceptions rather than giving error return.
void VSTWrapper::SaveXML(const wxFileName & fn) const
// may throw
{
   XMLFileWriter xmlFile{ fn.GetFullPath(), XO("Error Saving Effect Presets") };

   xmlFile.StartTag(wxT("vstprogrampersistence"));
   xmlFile.WriteAttr(wxT("version"), wxT("2"));

   xmlFile.StartTag(wxT("effect"));
   // Use internal name only in persistent information
   xmlFile.WriteAttr(wxT("name"), GetSymbol().Internal());
   xmlFile.WriteAttr(wxT("uniqueID"), mAEffect->uniqueID);
   xmlFile.WriteAttr(wxT("version"), mAEffect->version);
   xmlFile.WriteAttr(wxT("numParams"), mAEffect->numParams);

   xmlFile.StartTag(wxT("program"));
   xmlFile.WriteAttr(wxT("name"), wxEmptyString); //mProgram->GetValue());

   int clen = 0;
   if (mAEffect->flags & effFlagsProgramChunks)
   {
      void *chunk = NULL;

      // read-only dispatcher function
      clen = (int) constCallDispatcher(effGetChunk, 1, 0, &chunk, 0.0);
      if (clen != 0)
      {
         xmlFile.StartTag(wxT("chunk"));
         xmlFile.WriteSubTree(Base64::Encode(chunk, clen) + wxT('\n'));
         xmlFile.EndTag(wxT("chunk"));
      }
   }

   if (clen == 0)
   {
      for (int i = 0; i < mAEffect->numParams; i++)
      {
         xmlFile.StartTag(wxT("param"));

         xmlFile.WriteAttr(wxT("index"), i);
         xmlFile.WriteAttr(wxT("name"),
                           GetString(effGetParamName, i));
         xmlFile.WriteAttr(wxT("value"),
                           wxString::Format(wxT("%f"),
                           callGetParameter(i)));

         xmlFile.EndTag(wxT("param"));
      }
   }

   xmlFile.EndTag(wxT("program"));

   xmlFile.EndTag(wxT("effect"));

   xmlFile.EndTag(wxT("vstprogrampersistence"));

   xmlFile.Commit();
}

bool VSTWrapper::HandleXMLTag(const std::string_view& tag, const AttributesList &attrs)
{
   if (tag == "vstprogrampersistence")
   {
      for (auto pair : attrs)
      {
         auto attr = pair.first;
         auto value = pair.second;

         if (attr == "version")
         {
            if (!value.TryGet(mXMLVersion))
            {
               return false;
            }

            if (mXMLVersion < 1 || mXMLVersion > 2)
            {
               return false;
            }
         }
         else
         {
            return false;
         }
      }

      return true;
   }

   if (tag == "effect")
   {
      memset(&mXMLInfo, 0, sizeof(mXMLInfo));
      mXMLInfo.version = 1;
      mXMLInfo.pluginUniqueID = mAEffect->uniqueID;
      mXMLInfo.pluginVersion = mAEffect->version;
      mXMLInfo.numElements = mAEffect->numParams;

      for (auto pair : attrs)
      {
         auto attr = pair.first;
         auto value = pair.second;

         if (attr == "name")
         {
            wxString strValue = value.ToWString();

            if (strValue != GetSymbol().Internal())
            {
               auto msg = XO("This parameter file was saved from %s. Continue?")
                  .Format( strValue );
               int result = AudacityMessageBox(
                  msg,
                  XO("Confirm"),
                  wxYES_NO,
                  nullptr );
               if (result == wxNO)
               {
                  return false;
               }
            }
         }
         else if (attr == "version")
         {
            long version;
            if (!value.TryGet(version))
            {
               return false;
            }

            mXMLInfo.pluginVersion = (int) version;
         }
         else if (mXMLVersion > 1 && attr == "uniqueID")
         {
            long uniqueID;
            if (!value.TryGet(uniqueID))
            {
               return false;
            }

            mXMLInfo.pluginUniqueID = (int) uniqueID;
         }
         else if (mXMLVersion > 1 && attr == "numParams")
         {
            long numParams;
            if (!value.TryGet(numParams))
            {
               return false;
            }

            mXMLInfo.numElements = (int) numParams;
         }
         else
         {
            return false;
         }
      }

      return true;
   }

   if (tag == "program")
   {
      for (auto pair : attrs)
      {
         auto attr = pair.first;
         auto value = pair.second;

         if (attr == "name")
         {
            const wxString strValue = value.ToWString();

            if (strValue.length() > 24)
            {
               return false;
            }

            int ndx = 0; //mProgram->GetCurrentSelection();
            if (ndx == wxNOT_FOUND)
            {
               ndx = 0;
            }

            SetString(effSetProgramName, strValue, ndx);
         }
         else
         {
            return false;
         }
      }

      mInChunk = false;

      if (callDispatcher(effBeginLoadProgram, 0, 0, &mXMLInfo, 0.0) == -1)
      {
         return false;
      }

      callDispatcher(effBeginSetProgram, 0, 0, NULL, 0.0);

      mInSet = true;

      return true;
   }

   if (tag == "param")
   {
      long ndx = -1;
      double val = -1.0;

      for (auto pair : attrs)
      {
         auto attr = pair.first;
         auto value = pair.second;

         if (attr == "index")
         {
            if (!value.TryGet(ndx))
            {
               return false;
            }

            if (ndx < 0 || ndx >= mAEffect->numParams)
            {
               // Could be a different version of the effect...probably should
               // tell the user
               return false;
            }
         }
         // "name" attribute is ignored for params
         /* else if (attr == "name")
         {

            // Nothing to do with it for now
         }*/
         else if (attr == "value")
         {
            if (!value.TryGet(val))
            {
               return false;
            }

            if (val < 0.0 || val > 1.0)
            {
               return false;
            }
         }
      }

      if (ndx == -1 || val == -1.0)
      {
         return false;
      }

      callSetParameter(ndx, val);

      return true;
   }

   if (tag == "chunk")
   {
      mInChunk = true;
      return true;
   }

   return false;
}

void VSTWrapper::HandleXMLEndTag(const std::string_view& tag)
{
   if (tag == "chunk")
   {
      if (mChunk.length())
      {
         ArrayOf<char> buf{ mChunk.length() / 4 * 3 };

         int len = Base64::Decode(mChunk, buf.get());
         if (len)
         {
            callSetChunk(true, len, buf.get(), &mXMLInfo);
         }

         mChunk.clear();
      }
      mInChunk = false;
   }

   if (tag == "program")
   {
      if (mInSet)
      {
         callDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);

         mInSet = false;
      }
   }
}

void VSTWrapper::HandleXMLContent(const std::string_view& content)
{
   if (mInChunk)
   {
      mChunk += wxString(std::string(content)).Trim(true).Trim(false);
   }
}

XMLTagHandler *VSTWrapper::HandleXMLChild(const std::string_view& tag)
{
   if (tag == "vstprogrampersistence")
   {
      return this;
   }

   if (tag == "effect")
   {
      return this;
   }

   if (tag == "program")
   {
      return this;
   }

   if (tag == "param")
   {
      return this;
   }

   if (tag == "chunk")
   {
      return this;
   }

   return NULL;
}

void VSTWrapper::ForEachParameter(ParameterVisitor visitor) const
{
   for (int i = 0; i < mAEffect->numParams; i++)
   {
      wxString name = GetString(effGetParamName, i);
      if (name.empty())
      {
         name.Printf(wxT("parm_%d"), i);
      }
      else
         /* Easy fix for now for issue 3854, but this should be reconsidered
          There is the possibility that two parameter names might collide
          after normalizing.  A question is whether the normalizing was ever
          really needed for saving in a wxConfigFile.  Maybe not.  But then
          redefinition of the keys stored in the file may introduce versioning
          difficulties if there is an attempt to fix this in future Audacity.
          */
         name = CommandParameters::NormalizeName(name);

      ParameterInfo pi{ i, name };

      if (!visitor(pi))
         break;
   }
}

bool VSTWrapper::FetchSettings(VSTSettings& vstSettings, bool doFetch) const
{
   // Get the fallback ID-value parameters
   ForEachParameter
   (
      [&](const ParameterInfo& pi)
      {
         if (doFetch)
         {
            float val = callGetParameter(pi.mID);
            vstSettings.mParamsMap[pi.mName] = val;
         }
         else
         {
            vstSettings.mParamsMap[pi.mName] = std::nullopt;
         }
         return true;
      }
   );

   // These are here to be checked against for compatibility later
   vstSettings.mVersion   = mAEffect->version;
   vstSettings.mUniqueID  = mAEffect->uniqueID;
   vstSettings.mNumParams = mAEffect->numParams;

   // Get the chunk (if supported)
   vstSettings.mChunk.resize(0);

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      void* chunk = nullptr;
      int clen = (int)constCallDispatcher(effGetChunk, 1, 0, &chunk, 0.0);
      if (clen > 0 && chunk) {
         vstSettings.mChunk.resize(clen);
         memcpy(vstSettings.mChunk.data(), chunk, clen);
      }

      if (!doFetch)
      {
         // Don't keep the contents, but keep a sufficiently allocated string,
         // with some extra space in case chunk length might vary
         auto size = vstSettings.mChunk.size();
         vstSettings.mChunk.resize(0);
         vstSettings.mChunk.reserve(2 * size);
      }
   }

   return true;
}

bool VSTWrapper::StoreSettings(const VSTSettings& vstSettings) const
{
   // First, make sure settings are compatibile with the plugin
   if ((vstSettings.mUniqueID  != mAEffect->uniqueID)   ||
//       (vstSettings.mVersion   != mAEffect->version)    ||
       (vstSettings.mNumParams != mAEffect->numParams)      )
   {
      return false;
   }


   // Try using the chunk first (if available)
   auto &chunk = vstSettings.mChunk;
   if (!chunk.empty())
   {
      VstPatchChunkInfo info = { 1, mAEffect->uniqueID, mAEffect->version, mAEffect->numParams, "" };
      callSetChunk(true, chunk.size(), const_cast<char *>(chunk.data()), &info);
   }


   // Settings (like the message) may store both a chunk, and also accumulated
   // slider movements to reapply after the chunk change.  Or it might be
   // no chunk and id-value pairs only

   constCallDispatcher(effBeginSetProgram, 0, 0, NULL, 0.0);

   ForEachParameter
   (
      [&](const ParameterInfo& pi)
      {
         const auto itr = vstSettings.mParamsMap.find(pi.mName);
         if (itr != vstSettings.mParamsMap.end())
         {
            const float& value = *(itr->second);

            if (value >= -1.0 && value <= 1.0)
            {
               callSetParameter(pi.mID, value);
            }
         }
         return true;
      }
   );

   constCallDispatcher(effEndSetProgram, 0, 0, NULL, 0.0);

   return true;
}

ComponentInterfaceSymbol VSTWrapper::GetSymbol() const
{
   return mName;
}

std::unique_ptr<EffectInstance::Message>
VSTWrapper::MakeMessageFS(const VSTSettings &settings) const
{
   VSTMessage::ParamVector paramVector;
   paramVector.resize(mAEffect->numParams, std::nullopt);

   ForEachParameter
   (
      [&](const VSTWrapper::ParameterInfo& pi)
      {
         auto &slot = paramVector[pi.mID]; // operator [] may make a nullopt
         const auto iter = settings.mParamsMap.find(pi.mName),
            end = settings.mParamsMap.end();
         if (iter != end)
            slot = iter->second;
         return true;
      }
   );

   return std::make_unique<VSTMessage>(
      settings.mChunk /* vector copy */, std::move(paramVector));
}

void VSTWrapper::UpdateDisplay()
{
}

void VSTUIWrapper::Automate(int index, float value)
{
}

#endif // USE_VST
