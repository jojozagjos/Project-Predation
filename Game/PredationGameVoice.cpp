#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/DepthConvention.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/BuildInfo.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Surfaces.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Platform/Window.h"
#include "Engine/Audio/SoundDesign.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>


// Proximity voice: talking, hearing who is near, the microphone.

namespace pred
{

void PredationGame::StopTalking()
{
    if (!m_talking)
    {
        return;
    }
    m_talking = false;
    m_voiceLevel = 0.0f;
    // Not while the loopback test has it.
    //
    // There is one microphone and two things that want it, and the rule is that the test wins while
    // it is running -- UpdateVoice refuses to talk at all in that case. This is the same rule said
    // again at the point where the device is actually closed, because saying it only in the caller
    // is what let this go wrong: the test set a flag the voice path owned, the voice path read it a
    // frame later as "the player has stopped talking", and closed a device it did not have. Every
    // frame. A guard here costs one comparison and makes that impossible rather than unlikely.
    if (m_micTest)
    {
        return;
    }
    // Closed rather than left open and ignored. The operating system's microphone indicator is the
    // only thing a player has to go on, and it can only mean something if the device really is shut
    // when nobody is speaking.
    m_microphone.Stop();
}

void PredationGame::UpdateVoice(float dt)
{
    // --- Talking ---------------------------------------------------------------------------------
    const bool inSession = m_sessionMode != SessionMode::Offline;
    // Push to talk, or open mic if that is what somebody asked for. Either way the microphone is
    // only open while the game is being played and this player is alive: no talking from the pause
    // menu, the console, or a corpse.
    // Not while the loopback test is running either: one microphone, one owner.
    const bool allowed = inSession && cv_voiceEnabled.Get() && !m_micTest && m_screen == Screen::Playing &&
                         !m_paused && !m_app->IsConsoleOpen() && m_player.State().alive;
    const bool held = m_app->GetInput().IsActionDown("voice");
    // Open mic still needs the microphone open to hear whether anybody is speaking, so it opens on
    // being allowed and the gate below decides what is sent.
    const bool wants = allowed && (cv_voiceOpenMic.Get() || held);

    if (wants && !m_talking)
    {
        VoiceCapture::Settings settings;
        settings.sampleRate = 48000;
        settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
        if (!m_voiceCodec.Ready() && !m_voiceCodec.Init(VoiceCodec::Settings{}))
        {
            // No codec, no voice. Said once by the codec itself; nothing here retries every frame.
            m_talking = false;
        }
        else if (m_microphone.Start(settings))
        {
            m_talking = true;
        }
    }
    else if (!wants && m_talking)
    {
        StopTalking();
    }

    // Decayed rather than cleared, so the indicator does not strobe between packets: see
    // m_voiceSendingFor.
    m_voiceSendingFor = std::max(m_voiceSendingFor - dt, 0.0f);
    m_voiceSending = m_voiceSendingFor > 0.0f;
    if (m_talking)
    {
        std::vector<float> frame;
        std::vector<uint8_t> packet;
        // Everything the microphone has managed since the last frame, which at a steady frame rate
        // is one and occasionally two. Bounded so a stall cannot turn into a burst that arrives all
        // at once and is all stale by the time it is played.
        for (int guard = 0; guard < 8 && m_microphone.ReadFrame(frame); ++guard)
        {
            // Open mic decides per frame whether this is speech or the room. Push to talk has
            // already been decided by the key being down, so it skips this entirely.
            if (cv_voiceOpenMic.Get() &&
                !m_microphone.ShouldTransmit(m_microphone.LastLevel(), cv_voiceThreshold.Get(), dt))
            {
                continue;
            }
            if (!m_voiceCodec.Encode(frame.data(), frame.size(), packet))
            {
                break;
            }
            ++m_voiceSequence;
            m_voiceSendingFor = kVoiceSendingHold;
            m_voiceSending = true;
            if (m_sessionMode == SessionMode::Host)
            {
                // From the drone's speaker, when that is all that is left of them.
                glm::vec3 mouth = m_player.State().position;
                DroneOf(LocalPlayerId(), mouth);
                m_host.SendVoice(m_voiceSequence, packet, mouth);
                // Proximity chat is proximity for everything with ears. Talking near it is heard.
                MakeNoise(NoiseKind::Voice, mouth, NoiseReach::kVoice, LocalPlayerId());
                m_voiceMemory.Heard(LocalPlayerId(), packet, static_cast<float>(m_time));
            }
            else
            {
                m_client.SendVoice(m_voiceSequence, packet);
            }
        }
        m_voiceLevel = m_microphone.LastLevel();
    }

    // --- Listening -------------------------------------------------------------------------------
    //
    // Where a speaker is standing decides where their voice comes from, so it goes through exactly
    // the machinery a footstep does: the same attenuation, the same panning, the same distance cut.
    if (m_sessionMode == SessionMode::Host)
    {
        for (NetHost::VoiceHeard& heard : m_host.TakeVoice())
        {
            // Kept, for the match only, for the creatures that say things back.
            m_voiceMemory.Heard(heard.speaker, heard.frame, static_cast<float>(m_time));
            // Talking beside something with ears is heard by it, but only when we actually know
            // where the speaker is standing. A noise made at our own feet for somebody else's voice
            // would walk every creature on the map towards us.
            glm::vec3 where{0.0f};
            if (PlayerPositionIfKnown(heard.speaker, where))
            {
                MakeNoise(NoiseKind::Voice, where, NoiseReach::kVoice, heard.speaker);
            }
            if (heard.audible)
            {
                HearVoice(heard.speaker, heard.frame);
            }
        }
        UpdateMimicry(dt);
    }
    else if (m_sessionMode == SessionMode::Client)
    {
        for (NetClient::VoiceHeard& heard : m_client.TakeVoice())
        {
            HearVoice(heard.creature ? static_cast<uint8_t>(kCreatureSpeaker + heard.speaker) : heard.speaker, heard.frame);
        }
    }

    // A speaker who has stopped keeps their stream for a moment and then gives it back. Closing it
    // the instant a frame is missed would end the voice in every gap between two words.
    AudioEngine& audio = m_app->GetAudio();
    for (size_t i = 0; i < m_speakers.size();)
    {
        Speaker& speaker = *m_speakers[i];
        speaker.silentFor += dt;
        if (speaker.silentFor > 1.0f)
        {
            audio.CloseStream(speaker.stream);
            audio.Stop(speaker.voice);
            m_speakers.erase(m_speakers.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (speaker.voice != kInvalidVoice)
        {
            // In a cinematic everybody is heard as if beside you: nobody is anywhere in particular while it plays.
            const bool beside = m_cine.Active();
            audio.SetVoicePosition(speaker.voice, beside ? m_renderEye : SpeakerPosition(speaker));
            // Somebody on the other side of a wall is heard through it: muffled, not cut.
            audio.SetVoiceOcclusion(speaker.voice, beside ? 0.0f : OcclusionAt(SpeakerPosition(speaker)) * 0.8f);
        }
        ++i;
    }
}

// Where a voice comes from.
//
// Never from our own head. Answering "I do not know where they are" with the listener's own
// position is the same as answering "right here", and right here is the one place in the world
// where nothing is attenuated: it turned every voice the game could not place -- a player whose
// snapshot was a moment late, somebody who had just left -- into a whisper in the ear at full
// volume, which is how proximity chat stops being proximity chat.
glm::vec3 PredationGame::SpeakerPosition(Speaker& speaker) const
{
    glm::vec3 where{0.0f};
    // A creature saying something back: from its mouth, wherever it has got to.
    if (speaker.id >= kCreatureSpeaker)
    {
        for (const std::unique_ptr<Creature>& creature : m_creatures)
        {
            if (creature->NetId() == speaker.id - kCreatureSpeaker)
            {
                speaker.at = creature->Eye();
                speaker.located = true;
                return speaker.at;
            }
        }
    }
    else if (DroneOf(speaker.id, where) || PlayerPositionIfKnown(speaker.id, where))
    {
        speaker.at = where;
        speaker.located = true;
        return where;
    }
    if (speaker.located)
    {
        // Lost for a moment. They go on speaking from the last place we had them, which for a
        // dropped snapshot or two is within a step of where they really are.
        return speaker.at;
    }
    // Somebody we have never been able to put anywhere. They are placed at the far end of the
    // range, where the falloff is nought, so they are heard as silence until we know better.
    return m_player.State().position + glm::vec3(0.0f, 0.0f, kVoiceRange * 2.0f);
}

void PredationGame::HearVoice(uint8_t speaker, const std::vector<uint8_t>& frame)
{
    AudioEngine& audio = m_app->GetAudio();

    Speaker* found = nullptr;
    for (const std::unique_ptr<Speaker>& candidate : m_speakers)
    {
        if (candidate->id == speaker)
        {
            found = candidate.get();
            break;
        }
    }

    if (found == nullptr)
    {
        auto fresh = std::make_unique<Speaker>();
        fresh->id = speaker;
        if (!fresh->codec.Init(VoiceCodec::Settings{}))
        {
            return;
        }
        fresh->stream = audio.OpenStream(48000);
        if (fresh->stream == kInvalidStream)
        {
            return;
        }
        AudioEngine::PlayDesc desc;
        desc.stream = fresh->stream;
        desc.helmet = true; // a voice comes over the radio
        desc.position = SpeakerPosition(*fresh);
        desc.positioned = true;
        // Their volume, which is a setting and was not being read: the slider moved and nothing
        // happened. A voice at full gain on top of the game is also most of the way to the mix
        // clipping on its own.
        // With make-up gain: speech through the codec arrives well below everything else in the mix, and
        // at a slider of one people were hard to hear from across a room.
        // The levelling below does the making-up; this is the slider, and a little over.
        desc.gain = std::clamp(cv_voiceVolume.Get(), 0.0f, 2.0f) * 1.5f;
        // A voice carries further than a footstep and falls off gently: the point of proximity chat
        // is knowing roughly where somebody is, and a hard cut turns that into a switch.
        desc.nearDistance = 8.0f;
        desc.farDistance = kVoiceRange;
        fresh->voice = audio.Play(desc);
        m_speakers.push_back(std::move(fresh));
        found = m_speakers.back().get();
    }

    found->silentFor = 0.0f;
    std::vector<float> samples;
    if (found->codec.Decode(frame.data(), frame.size(), samples) && !samples.empty())
    {
        LevelVoice(samples, found->level);
        audio.PushStream(found->stream, samples.data(), samples.size());
    }
}

void PredationGame::UpdateMicrophoneTest(float dt)
{
    // Hear yourself, so the threshold and the device can be set without a second person.
    //
    // Everything about a microphone setting is invisible until somebody speaks into it and somebody
    // else says whether they heard anything. Picking a device from a list of four names, one of which
    // is the monitor, is a guess; setting a gate threshold by watching a bar is a better guess. This
    // closes the loop: what the other players would hear comes back out of the speakers, through the
    // same gate and the same codec, so what is being judged is the thing that will actually be sent.
    //
    // Played flat rather than positioned. It is not coming from anywhere in the world.
    AudioEngine& audio = m_app->GetAudio();
    if (!m_micTest)
    {
        if (m_micTestStream != kInvalidStream)
        {
            audio.CloseStream(m_micTestStream);
            m_micTestStream = kInvalidStream;
            m_microphone.Stop();
        }
        return;
    }

    if (m_micTestStream == kInvalidStream)
    {
        VoiceCapture::Settings settings;
        settings.sampleRate = 48000;
        settings.deviceId = static_cast<uint32_t>(std::max(cv_voiceDevice.Get(), 0));
        if (!m_microphone.Start(settings))
        {
            m_micTest = false;
            return;
        }
        m_micTestStream = audio.OpenStream(48000);
        if (m_micTestStream == kInvalidStream)
        {
            m_microphone.Stop();
            m_micTest = false;
            return;
        }
        AudioEngine::PlayDesc desc;
        desc.stream = m_micTestStream;
        desc.helmet = true;
        desc.positioned = false;
        desc.gain = 1.0f;
        audio.Play(desc);
    }

    std::vector<float> frame;
    while (m_microphone.ReadFrame(frame))
    {
        m_voiceLevel = m_microphone.LastLevel();
        // Through the same gate the real thing uses, so a threshold set here is the threshold that
        // decides whether a word reaches anybody.
        const bool sending = !cv_voiceOpenMic.Get() ||
                             m_microphone.ShouldTransmit(m_voiceLevel, cv_voiceThreshold.Get(), dt);
        // `m_voiceSending`, not `m_talking`, and the difference is the whole of why this never
        // worked.
        //
        // `m_talking` means "the voice path owns the microphone". UpdateVoice runs first, and every
        // frame it saw m_talking set while its own conditions said it should not be talking -- its
        // conditions exclude the loopback test on purpose -- so it concluded the player had stopped
        // and called StopTalking, which destroys the capture device. The test then read from a
        // device that had just been closed and got nothing, the panel said "microphone shut", and
        // the game spent every frame tearing down and reopening a Windows capture stream, which is
        // what the output audio was being wrecked by.
        //
        // The test does not own m_talking and must not set it. What it has to report is whether the
        // gate is passing, which is a different thing and has its own flag.
        if (sending)
        {
            m_voiceSendingFor = kVoiceSendingHold;
        }
        m_voiceSending = m_voiceSendingFor > 0.0f;
        if (sending)
        {
            // Levelled as it will be at the other end, so what is heard here is what they hear.
            LevelVoice(frame, m_micTestLevel);
            audio.PushStream(m_micTestStream, frame.data(), frame.size());
        }
    }
}


} // namespace pred
