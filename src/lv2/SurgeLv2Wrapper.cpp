#include "SurgeLv2Wrapper.h"
#include "SurgeLv2Util.h"
#include "SurgeLv2Ui.h" // for editor's setParameterAutomated
#include <cstring>

///
SurgeLv2Wrapper::SurgeLv2Wrapper(double sampleRate)
    : _synthesizer(new SurgeSynthesizer(this)), _dataLocation(new void*[NumPorts]()),
      _oldControlValues(new float[n_total_params]()), _sampleRate(sampleRate)
{
   // needed?
   _synthesizer->time_data.ppqPos = 0;
}

SurgeLv2Wrapper::~SurgeLv2Wrapper()
{}

void SurgeLv2Wrapper::updateDisplay()
{
   // should I do anything particular here?
}

void SurgeLv2Wrapper::setParameterAutomated(int externalparam, float value)
{
   // this is called from the editor; in Vst, it's supposed to be called also
   // on custom controllers, but I disable that in SurgeSynthesizer LV2.
   // By LV2's rules, a DSP cannot modify value of its control inputs.
   _editor->setParameterAutomated(externalparam, value);
}

LV2_Handle SurgeLv2Wrapper::instantiate(const LV2_Descriptor* descriptor,
                                        double sample_rate,
                                        const char* bundle_path,
                                        const LV2_Feature* const* features)
{
   std::unique_ptr<SurgeLv2Wrapper> self(new SurgeLv2Wrapper(sample_rate));

   SurgeSynthesizer* synth = self->_synthesizer.get();
   synth->setSamplerate(sample_rate);

   auto* featureUridMap = (const LV2_URID_Map*)SurgeLv2::requireFeature(LV2_URID__map, features);

   self->_uridMidiEvent = featureUridMap->map(featureUridMap->handle, LV2_MIDI__MidiEvent);
   self->_uridAtomBlank = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Blank);
   self->_uridAtomObject = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Object);
   self->_uridAtomDouble = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Double);
   self->_uridAtomFloat = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Float);
   self->_uridAtomInt = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Int);
   self->_uridAtomLong = featureUridMap->map(featureUridMap->handle, LV2_ATOM__Long);
   self->_uridTimePosition = featureUridMap->map(featureUridMap->handle, LV2_TIME__Position);
   self->_uridTime_beatsPerMinute =
       featureUridMap->map(featureUridMap->handle, LV2_TIME__beatsPerMinute);
   self->_uridTime_speed = featureUridMap->map(featureUridMap->handle, LV2_TIME__speed);
   self->_uridTime_beat = featureUridMap->map(featureUridMap->handle, LV2_TIME__beat);

   return (LV2_Handle)self.release();
}

void SurgeLv2Wrapper::connectPort(LV2_Handle instance, uint32_t port, void* data_location)
{
   SurgeLv2Wrapper* self = (SurgeLv2Wrapper*)instance;
   self->_dataLocation[port] = data_location;
}

void SurgeLv2Wrapper::activate(LV2_Handle instance)
{
   SurgeLv2Wrapper* self = (SurgeLv2Wrapper*)instance;
   SurgeSynthesizer* s = self->_synthesizer.get();

   self->_blockPos = 0;

   for (unsigned pNth = 0; pNth < n_total_params; ++pNth)
   {
      unsigned index = s->remapExternalApiToInternalId(pNth);
      self->_oldControlValues[pNth] = s->getParameter01(index);
   }

   s->audio_processing_active = true;
}

void SurgeLv2Wrapper::run(LV2_Handle instance, uint32_t sample_count)
{
   SurgeLv2Wrapper* self = (SurgeLv2Wrapper*)instance;
   SurgeSynthesizer* s = self->_synthesizer.get();
   double sampleRate = self->_sampleRate;

   self->_fpuState.set();

   for (unsigned pNth = 0; pNth < n_total_params; ++pNth)
   {
      float portValue = *(float*)(self->_dataLocation[pNth]);
      if (portValue != self->_oldControlValues[pNth])
      {
         unsigned index = s->remapExternalApiToInternalId(pNth);
         s->setParameter01(index, portValue);
         self->_oldControlValues[pNth] = portValue;
      }
   }

   s->process_input = false /*(!plug_is_synth || input_connected)*/;

   bool isPlaying = SurgeLv2::isNotZero(self->_timePositionSpeed);
   bool hasValidTempo = SurgeLv2::isGreaterThanZero(self->_timePositionTempoBpm);
   bool hasValidPosition = self->_timePositionBeat >= 0.0;

   if (hasValidTempo)
   {
      if (isPlaying)
         s->time_data.tempo = self->_timePositionTempoBpm * std::fabs(self->_timePositionSpeed);
   }
   else
   {
      s->time_data.tempo = 120.0;
   }

   if (isPlaying && hasValidPosition)
      s->time_data.ppqPos = self->_timePositionBeat; // TODO is it correct?

   auto* eventSequence = (const LV2_Atom_Sequence*)self->_dataLocation[pEvents];
   const LV2_Atom_Event* eventIter = lv2_atom_sequence_begin(&eventSequence->body);
   const LV2_Atom_Event* event = SurgeLv2::popNextEvent(eventSequence, &eventIter);

   const float* inputs[NumInputs];
   float* outputs[NumOutputs];

   for (uint32_t i = 0; i < NumInputs; ++i)
      inputs[i] = (const float*)self->_dataLocation[pAudioInput1 + i];
   for (uint32_t i = 0; i < NumOutputs; ++i)
      outputs[i] = (float*)self->_dataLocation[pAudioOutput1 + i];

   for (uint32_t i = 0; i < sample_count; ++i)
   {
      if (self->_blockPos == 0)
      {
         // move clock
         s->time_data.ppqPos += (double)BLOCK_SIZE * s->time_data.tempo / (60. * sampleRate);

         // process events for the current block
         while (event && i >= event->time.frames)
         {
            self->handleEvent(event);
            event = SurgeLv2::popNextEvent(eventSequence, &eventIter);
         }

         // run sampler engine for the current block
         s->process();
      }

      if (s->process_input)
      {
         int inp;
         for (inp = 0; inp < NumInputs; inp++)
         {
            s->input[inp][self->_blockPos] = inputs[inp][i];
         }
      }

      int outp;
      for (outp = 0; outp < NumOutputs; outp++)
      {
         outputs[outp][i] = (float)s->output[outp][self->_blockPos]; // replacing
      }

      self->_blockPos++;
      if (self->_blockPos >= BLOCK_SIZE)
         self->_blockPos = 0;
   }

   while (event)
   {
      self->handleEvent(event);
      event = SurgeLv2::popNextEvent(eventSequence, &eventIter);
   }

   self->_fpuState.restore();
}

void SurgeLv2Wrapper::handleEvent(const LV2_Atom_Event* event)
{
   if (event->body.type == _uridMidiEvent)
   {
      const char* midiData = (char*)event + sizeof(*event);
      int status = midiData[0] & 0xf0;
      int channel = midiData[0] & 0x0f;
      if (status == 0x90 || status == 0x80) // we only look at notes
      {
         int newnote = midiData[1] & 0x7f;
         int velo = midiData[2] & 0x7f;
         if ((status == 0x80) || (velo == 0))
         {
            _synthesizer->releaseNote((char)channel, (char)newnote, (char)velo);
         }
         else
         {
            _synthesizer->playNote((char)channel, (char)newnote, (char)velo, 0 /*event->detune*/);
         }
      }
      else if (status == 0xe0) // pitch bend
      {
         long value = (midiData[1] & 0x7f) + ((midiData[2] & 0x7f) << 7);
         _synthesizer->pitchBend((char)channel, value - 8192);
      }
      else if (status == 0xB0) // controller
      {
         if (midiData[1] == 0x7b || midiData[1] == 0x7e)
            _synthesizer->allNotesOff(); // all notes off
         else
            _synthesizer->channelController((char)channel, midiData[1] & 0x7f, midiData[2] & 0x7f);
      }
      else if (status == 0xC0) // program change
      {
         _synthesizer->programChange((char)channel, midiData[1] & 0x7f);
      }
      else if (status == 0xD0) // channel aftertouch
      {
         _synthesizer->channelAftertouch((char)channel, midiData[1] & 0x7f);
      }
      else if (status == 0xA0) // poly aftertouch
      {
         _synthesizer->polyAftertouch((char)channel, midiData[1] & 0x7f, midiData[2] & 0x7f);
      }
      else if ((status == 0xfc) || (status == 0xff)) //  MIDI STOP or reset
      {
         _synthesizer->allNotesOff();
      }
   }
   else if ((event->body.type == _uridAtomBlank || event->body.type == _uridAtomObject) &&
            (((const LV2_Atom_Object*)&event->body)->body.otype) == _uridTimePosition)
   {
      const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&event->body;

      LV2_Atom* beatsPerMinute = nullptr;
      LV2_Atom* speed = nullptr;
      LV2_Atom* beat = nullptr;

      lv2_atom_object_get(obj, _uridTime_beatsPerMinute, &beatsPerMinute, _uridTime_speed, &speed,
                          _uridTime_beat, &beat, 0);

      auto atomGetNumber = [this](const LV2_Atom* atom, double* number) -> bool {
         if (atom->type == _uridAtomDouble)
            *number = ((const LV2_Atom_Double*)atom)->body;
         else if (atom->type == _uridAtomFloat)
            *number = ((const LV2_Atom_Float*)atom)->body;
         else if (atom->type == _uridAtomInt)
            *number = ((const LV2_Atom_Int*)atom)->body;
         else if (atom->type == _uridAtomLong)
            *number = ((const LV2_Atom_Long*)atom)->body;
         else
            return false;
         return true;
      };

      if (speed)
         atomGetNumber(speed, &_timePositionSpeed);
      if (beatsPerMinute)
         atomGetNumber(beatsPerMinute, &_timePositionTempoBpm);
      if (beat)
         atomGetNumber(beat, &_timePositionBeat);
   }
}

void SurgeLv2Wrapper::deactivate(LV2_Handle instance)
{
   SurgeLv2Wrapper* self = (SurgeLv2Wrapper*)instance;

   self->_synthesizer->allNotesOff();
   self->_synthesizer->audio_processing_active = false;
}

void SurgeLv2Wrapper::cleanup(LV2_Handle instance)
{
   SurgeLv2Wrapper* self = (SurgeLv2Wrapper*)instance;
   delete self;
}

const void* SurgeLv2Wrapper::extensionData(const char* uri)
{
   return nullptr;
}

LV2_Descriptor SurgeLv2Wrapper::createDescriptor()
{
   LV2_Descriptor desc = {};
   desc.URI = "https://github.com/surge-synthesizer/surge";
   desc.instantiate = &instantiate;
   desc.connect_port = &connectPort;
   desc.activate = &activate;
   desc.run = &run;
   desc.deactivate = &deactivate;
   desc.cleanup = &cleanup;
   desc.extension_data = &extensionData;
   return desc;
}

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
   if (index == 0)
   {
      static LV2_Descriptor desc = SurgeLv2Wrapper::createDescriptor();
      return &desc;
   }
   return nullptr;
}
