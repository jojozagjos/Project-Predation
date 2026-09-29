#include "Game/Cinematic/CinematicEditor.h"

#include "Engine/Assets/ModelAsset.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace pred
{
namespace
{

constexpr float kFrame = 1.0f / 30.0f;
constexpr float kLabelWidth = 150.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kRulerHeight = 22.0f;
constexpr float kInspectorWidth = 390.0f;
constexpr float kTimelineHeight = 320.0f;
constexpr size_t kUndoDepth = 200;

constexpr ImU32 kCameraColour = IM_COL32(236, 196, 84, 255);
constexpr ImU32 kShotColour = IM_COL32(84, 118, 176, 255);
constexpr ImU32 kActorColour = IM_COL32(88, 196, 206, 255);
constexpr ImU32 kPathColour = IM_COL32(64, 140, 112, 255);
constexpr ImU32 kClipColour = IM_COL32(170, 214, 120, 255);
constexpr ImU32 kSoundColour = IM_COL32(214, 132, 212, 255);
constexpr ImU32 kMarkerColour = IM_COL32(236, 104, 88, 255);
constexpr ImU32 kTextColour = IM_COL32(150, 150, 164, 255);
constexpr ImU32 kParticleColour = IM_COL32(200, 128, 64, 255);
constexpr ImU32 kLightColour = IM_COL32(250, 236, 150, 255);
constexpr ImU32 kFloatColour = IM_COL32(150, 176, 214, 255);
constexpr ImU32 kPlayheadColour = IM_COL32(255, 86, 70, 255);

// The lists of plain values over time, as the timeline shows them, and what each is with no keys at all.
constexpr int kFloatLists = 6;
constexpr const char* kFloatNames[kFloatLists] = {"Shake", "Shake speed", "Fade", "Bars", "Fog pushed back", "Dark lifted"};
constexpr float kFloatNone[kFloatLists] = {0.0f, 14.0f, 0.0f, 0.0f, 1.0f, 1.0f};

std::vector<FloatKey>& FloatList(Cinematic& cinematic, int which)
{
    switch (which)
    {
    case 0: return cinematic.shake;
    case 1: return cinematic.shakeSpeed;
    case 2: return cinematic.fade;
    case 3: return cinematic.letterbox;
    case 4: return cinematic.fogScale;
    default: return cinematic.ambientScale;
    }
}

const std::vector<FloatKey>& FloatList(const Cinematic& cinematic, int which)
{
    return FloatList(const_cast<Cinematic&>(cinematic), which);
}

bool InRange(int index, size_t size)
{
    return index >= 0 && static_cast<size_t>(index) < size;
}

// Back in time order the way Cinematic::Tidy puts them -- equal times keep their order -- saying where the one at `keep`
// went.
template <typename T, typename TimeOf>
int SortKeeping(std::vector<T>& list, int keep, TimeOf timeOf)
{
    std::vector<int> order(list.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return timeOf(list[static_cast<size_t>(a)]) < timeOf(list[static_cast<size_t>(b)]); });
    std::vector<T> sorted;
    sorted.reserve(list.size());
    int found = keep;
    for (size_t i = 0; i < order.size(); ++i)
    {
        sorted.push_back(std::move(list[static_cast<size_t>(order[i])]));
        if (order[i] == keep)
        {
            found = static_cast<int>(i);
        }
    }
    list = std::move(sorted);
    return found;
}

template <typename T>
int SortByTime(std::vector<T>& list, int keep)
{
    return SortKeeping(list, keep, [](const T& item) { return item.time; });
}

bool InputString(const char* label, std::string& value)
{
    char buffer[256] = {};
    value.copy(buffer, std::min(value.size(), sizeof(buffer) - 1));
    if (ImGui::InputText(label, buffer, sizeof(buffer)))
    {
        value = buffer;
        return true;
    }
    return false;
}

std::string Joined(const std::vector<std::string>& lines)
{
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        text += (i > 0 ? "\n" : "") + lines[i];
    }
    return text;
}

std::vector<std::string> Split(const std::string& text)
{
    std::vector<std::string> lines;
    size_t from = 0;
    while (from <= text.size())
    {
        const size_t end = text.find('\n', from);
        lines.push_back(text.substr(from, end == std::string::npos ? std::string::npos : end - from));
        if (end == std::string::npos)
        {
            break;
        }
        from = end + 1;
    }
    return lines;
}

// A name nobody has yet: "camera 3".
template <typename List, typename NameOf>
std::string Unused(const std::string& stem, const List& list, NameOf nameOf)
{
    for (int n = 1;; ++n)
    {
        const std::string name = stem + " " + std::to_string(n);
        if (std::none_of(list.begin(), list.end(), [&](const auto& item) { return nameOf(item) == name; }))
        {
            return name;
        }
    }
}

// A file name the game can find again: letters, figures, underscores and dashes.
bool Fileable(const std::string& name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
}

constexpr const char* kEaseNames[] = {"Linear", "Step", "Ease in", "Ease out", "Ease in and out", "Curve"};

// The hand-shaped curve nearest each named ease, to start one from what it was.
void CurveFor(Ease& ease)
{
    switch (ease.kind)
    {
    case Ease::Kind::Linear: ease.a = {0.0f, 0.0f}, ease.b = {1.0f, 1.0f}; break;
    case Ease::Kind::Step: ease.a = {1.0f, 0.0f}, ease.b = {1.0f, 0.0f}; break;
    case Ease::Kind::In: ease.a = {0.32f, 0.0f}, ease.b = {0.67f, 0.0f}; break;
    case Ease::Kind::Out: ease.a = {0.33f, 1.0f}, ease.b = {0.68f, 1.0f}; break;
    case Ease::Kind::InOut: ease.a = {0.45f, 0.0f}, ease.b = {0.55f, 1.0f}; break;
    case Ease::Kind::Curve: break;
    }
}

ImU32 Faded(ImU32 colour, int alpha)
{
    return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}

} // namespace

// --- Opening, closing, the frame ---------------------------------------------------------------------------

void CinematicEditor::Close()
{
    m_open = false;
    m_dragging = false;
    m_scrubbing = false;
}

void CinematicEditor::LookFreely(const CameraState& from)
{
    if (m_throughCinematic)
    {
        ViewFrom(from);
        m_throughCinematic = false;
    }
}

bool CinematicEditor::MayClose()
{
    // An Escape that closed a menu closed only the menu.
    if (m_menuOpen)
    {
        return false;
    }
    if (!Unsaved() || (m_closeArmed && m_statusAge < 4.0f))
    {
        m_closeArmed = false;
        return true;
    }
    m_closeArmed = true;
    m_status = "Not saved: Escape again to leave it anyway, Ctrl+S to save";
    m_statusAge = 0.0f;
    return false;
}

bool CinematicEditor::Preview(glm::vec2& min, glm::vec2& max) const
{
    if (!m_open || m_hidden || m_inspectorLeft <= 0.0f || m_timelineTop <= 0.0f)
    {
        return false;
    }
    // The biggest the screen's shape fits into what is left, in the middle of it across and at the top.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::max(m_inspectorLeft - viewport->Pos.x, 64.0f);
    const float height = std::max(m_timelineTop - viewport->Pos.y, 36.0f);
    const float aspect = viewport->Size.x / std::max(viewport->Size.y, 1.0f);
    const float w = std::min(width, height * aspect);
    const float h = w / aspect;
    min = {viewport->Pos.x + (width - w) * 0.5f, viewport->Pos.y + (height - h) * 0.5f};
    max = {min.x + w, min.y + h};
    return true;
}

std::string CinematicEditor::SelectedCamera(const Cinematic& cinematic) const
{
    if ((m_selected.kind == Pick::Camera || m_selected.kind == Pick::CameraKey) && InRange(m_selected.track, cinematic.cameras.size()))
    {
        return cinematic.cameras[static_cast<size_t>(m_selected.track)].name;
    }
    if (m_selected.kind == Pick::Shot && InRange(m_selected.item, cinematic.shots.size()))
    {
        return cinematic.shots[static_cast<size_t>(m_selected.item)].camera;
    }
    return {};
}

void CinematicEditor::AtEnd(Context& context)
{
    if (m_loop)
    {
        context.player->Seek(0.0f, *context.scene, *context.host);
        context.player->SetPaused(false);
    }
}

void CinematicEditor::Draw(Context& context)
{
    if (!m_open || context.player == nullptr)
    {
        return;
    }
    m_statusAge += ImGui::GetIO().DeltaTime;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Nothing playing to edit: which to open, or a new one.
    if (!context.player->Active())
    {
        ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f}, ImGuiCond_Appearing,
                                {0.5f, 0.5f});
        if (ImGui::Begin("Cinematic editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextUnformatted("Which cinematic?");
            for (const auto& [name, cinematic] : *context.library)
            {
                if (ImGui::Button(name.c_str()))
                {
                    Replay(context, cinematic, 0.0f);
                    context.player->SetPaused(true);
                    Adopt(context);
                }
            }
            ImGui::Separator();
            ImGui::InputText("##newname", m_newName, sizeof(m_newName));
            ImGui::SameLine();
            if (ImGui::Button("New") && Fileable(m_newName))
            {
                Cinematic fresh;
                fresh.name = m_newName;
                CameraTrack camera;
                camera.name = "main";
                CameraKey key;
                KeyFromView(context, key, "", 0.0f);
                camera.keys.push_back(key);
                fresh.cameras.push_back(camera);
                fresh.shots.push_back({0.0f, "main", 0.0f, {}});
                Replay(context, fresh, 0.0f);
                context.player->SetPaused(true);
                Adopt(context);
            }
        }
        ImGui::End();
        return;
    }
    if (Edited(context).name != m_editing)
    {
        Adopt(context);
    }
    if (!Valid(Edited(context), m_selected))
    {
        m_selected = {};
    }

    // Hidden, the picture has the screen, as in the game; H brings the editor back.
    if (m_hidden)
    {
        ImGui::GetForegroundDrawList()->AddText({viewport->Pos.x + 12.0f, viewport->Pos.y + viewport->Size.y - 24.0f}, IM_COL32(255, 210, 120, 160),
                                                "Editor hidden: H to bring it back, Space to play and pause");
        Shortcuts(context);
        return;
    }

    // The timeline along the bottom and the inspector down the right: the picture keeps the rest.
    ImGui::SetNextWindowPos({viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - kTimelineHeight}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({viewport->WorkSize.x - kInspectorWidth, kTimelineHeight}, ImGuiCond_Appearing);
    const bool dirty = m_pending || m_committed != m_saved;
    const std::string title = "Cinematic -- " + Edited(context).name + (dirty ? " *" : "") + "###cine_timeline";
    if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        m_timelineTop = ImGui::GetWindowPos().y;
        DrawToolbar(context);
        DrawTimeline(context);
    }
    ImGui::End();

    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - kInspectorWidth, viewport->WorkPos.y}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize({kInspectorWidth, viewport->WorkSize.y}, ImGuiCond_Appearing);
    if (ImGui::Begin("Inspector###cine_inspector", nullptr, ImGuiWindowFlags_NoCollapse))
    {
        m_inspectorLeft = ImGui::GetWindowPos().x;
        DrawInspector(context);
    }
    ImGui::End();

    Shortcuts(context);
    m_menuOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    // A change is kept for undoing once it is finished with: a drag let go of, a field left.
    if (m_pending && !m_dragging && !ImGui::IsAnyItemActive())
    {
        Commit(context);
    }
}

// --- History ----------------------------------------------------------------------------------------

void CinematicEditor::Changed(Context& context)
{
    Resort(Edited(context));
    context.player->Refresh(*context.scene, *context.host);
    m_pending = true;
}

void CinematicEditor::Commit(Context& context)
{
    m_pending = false;
    std::string now = Edited(context).ToJsonText();
    if (now != m_committed)
    {
        m_undo.push_back(std::move(m_committed));
        if (m_undo.size() > kUndoDepth)
        {
            m_undo.erase(m_undo.begin());
        }
        m_redo.clear();
        m_committed = std::move(now);
    }
    // An actor added, taken away or changed: its model has to be put in the world, which starting again does.
    if (m_needsReplay)
    {
        m_needsReplay = false;
        Replay(context, Edited(context), context.player->Time());
    }
}

void CinematicEditor::Adopt(Context& context)
{
    m_editing = Edited(context).name;
    m_committed = Edited(context).ToJsonText();
    const auto found = context.library->find(m_editing);
    if (found != context.library->end())
    {
        Cinematic saved = found->second;
        saved.Tidy();
        m_saved = saved.ToJsonText();
    }
    else
    {
        m_saved.clear();
    }
    m_undo.clear();
    m_redo.clear();
    m_pending = false;
    m_needsReplay = false;
    m_selected = {};
    m_dragging = false;
    m_fitted = false;
}

void CinematicEditor::Apply(Context& context, const std::string& text)
{
    Cinematic restored;
    if (!restored.FromJsonText(text))
    {
        return;
    }
    const Cinematic& now = Edited(context);
    const bool sameActors = std::equal(now.actors.begin(), now.actors.end(), restored.actors.begin(), restored.actors.end(),
                                       [](const ActorDef& a, const ActorDef& b) { return a.name == b.name && a.model == b.model && a.bind == b.bind; });
    m_editing = restored.name;
    if (sameActors)
    {
        Edited(context) = std::move(restored);
        context.player->Refresh(*context.scene, *context.host);
    }
    else
    {
        Replay(context, restored, context.player->Time());
    }
    if (!Valid(Edited(context), m_selected))
    {
        m_selected = {};
    }
}

void CinematicEditor::Undo(Context& context)
{
    if (m_pending)
    {
        Commit(context);
    }
    if (m_undo.empty())
    {
        m_status = "Nothing to undo";
        m_statusAge = 0.0f;
        return;
    }
    m_redo.push_back(m_committed);
    m_committed = m_undo.back();
    m_undo.pop_back();
    Apply(context, m_committed);
}

void CinematicEditor::Redo(Context& context)
{
    if (m_pending)
    {
        Commit(context);
    }
    if (m_redo.empty())
    {
        return;
    }
    m_undo.push_back(m_committed);
    m_committed = m_redo.back();
    m_redo.pop_back();
    Apply(context, m_committed);
}

void CinematicEditor::Replay(Context& context, const Cinematic& cinematic, float from)
{
    const bool playing = context.player->GetState() == CinematicPlayer::State::Playing;
    const Cinematic copy = cinematic; // what it came from may be the player's own, which starting again replaces
    context.replay(copy, from);
    if (!playing)
    {
        context.player->SetPaused(true);
    }
    m_editing = copy.name;
}

// --- Time ---------------------------------------------------------------------------------------------

void CinematicEditor::Seek(Context& context, float time)
{
    context.player->Seek(std::clamp(time, 0.0f, Edited(context).duration), *context.scene, *context.host);
}

void CinematicEditor::PlayPause(Context& context)
{
    CinematicPlayer& player = *context.player;
    switch (player.GetState())
    {
    case CinematicPlayer::State::Playing: player.SetPaused(true); break;
    case CinematicPlayer::State::Paused: player.SetPaused(false); break;
    case CinematicPlayer::State::Finished:
        Seek(context, 0.0f);
        player.SetPaused(false);
        break;
    case CinematicPlayer::State::Stopped: break;
    }
}

float CinematicEditor::Snapped(float time, float playhead) const
{
    // Shift held lets it go anywhere.
    if (!m_snap || ImGui::GetIO().KeyShift)
    {
        return time;
    }
    // To the playhead when close to it, and to the step otherwise.
    if (std::abs(time - playhead) * m_pixelsPerSecond < 6.0f)
    {
        return playhead;
    }
    return std::round(time / m_snapStep) * m_snapStep;
}

// --- The toolbar --------------------------------------------------------------------------------------

void CinematicEditor::DrawToolbar(Context& context)
{
    Cinematic& cinematic = Edited(context);
    CinematicPlayer& player = *context.player;
    const bool dirty = m_pending || m_committed != m_saved;

    // Which cinematic: another one, leaving unsaved changes, is asked about first.
    static std::string s_switchTo;
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo("##which", cinematic.name.c_str()))
    {
        for (const auto& [name, other] : *context.library)
        {
            if (ImGui::Selectable(name.c_str(), name == cinematic.name) && name != cinematic.name)
            {
                s_switchTo = name;
            }
        }
        ImGui::EndCombo();
    }
    if (!s_switchTo.empty())
    {
        if (dirty)
        {
            ImGui::OpenPopup("Leave unsaved changes?");
        }
        else if (const auto found = context.library->find(s_switchTo); found != context.library->end())
        {
            Replay(context, found->second, 0.0f);
            Adopt(context);
            s_switchTo.clear();
        }
    }
    if (ImGui::BeginPopupModal("Leave unsaved changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            s_switchTo.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::Text("%s has changes that are not saved.", cinematic.name.c_str());
        if (ImGui::Button("Leave them"))
        {
            if (const auto found = context.library->find(s_switchTo); found != context.library->end())
            {
                Replay(context, found->second, 0.0f);
                Adopt(context);
            }
            s_switchTo.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stay"))
        {
            s_switchTo.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        if (m_pending)
        {
            Commit(context);
        }
        if (!Fileable(cinematic.name))
        {
            m_status = "The name has to be letters, figures, _ and - to be a file";
        }
        else
        {
            Cinematic copy = cinematic;
            copy.Tidy();
            const std::filesystem::path file = context.folder / (copy.name + ".json");
            if (copy.SaveToFile(file))
            {
                (*context.library)[copy.name] = copy;
                m_saved = copy.ToJsonText();
                m_committed = m_saved;
                m_status = "Saved " + file.filename().string();
            }
            else
            {
                m_status = "Could not write " + file.string();
            }
        }
        m_statusAge = 0.0f;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty || context.library->count(cinematic.name) == 0);
    if (ImGui::Button("Revert"))
    {
        // Back to the file -- undoably.
        if (m_pending)
        {
            Commit(context);
        }
        Cinematic saved = context.library->at(cinematic.name);
        saved.Tidy();
        m_undo.push_back(m_committed);
        m_redo.clear();
        m_committed = saved.ToJsonText();
        Apply(context, m_committed);
        m_status = "Back to the saved one";
        m_statusAge = 0.0f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New / save as"))
    {
        std::snprintf(m_newName, sizeof(m_newName), "%s", cinematic.name.c_str());
        ImGui::OpenPopup("##newas");
    }
    if (ImGui::BeginPopup("##newas"))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Name", m_newName, sizeof(m_newName));
        const bool fileable = Fileable(m_newName);
        ImGui::BeginDisabled(!fileable);
        if (ImGui::Button("Save a copy as this"))
        {
            cinematic.name = m_newName;
            m_editing = cinematic.name;
            Changed(context);
            Commit(context);
            Cinematic copy = cinematic;
            copy.Tidy();
            if (copy.SaveToFile(context.folder / (copy.name + ".json")))
            {
                (*context.library)[copy.name] = copy;
                m_saved = copy.ToJsonText();
                m_status = "Saved as " + copy.name;
            }
            m_statusAge = 0.0f;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Start a new one"))
        {
            Cinematic fresh;
            fresh.name = m_newName;
            CameraTrack camera;
            camera.name = "main";
            CameraKey key;
            KeyFromView(context, key, "", 0.0f);
            camera.keys.push_back(key);
            fresh.cameras.push_back(camera);
            fresh.shots.push_back({0.0f, "main", 0.0f, {}});
            Replay(context, fresh, 0.0f);
            context.player->SetPaused(true);
            Adopt(context);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        if (!fileable)
        {
            ImGui::TextDisabled("Letters, figures, _ and - only.");
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Add"))
    {
        ImGui::OpenPopup("##add");
    }
    DrawAddMenu(context);

    // The view: through the cinematic's cameras, or looking round freely.
    ImGui::SameLine();
    if (ImGui::Button(m_throughCinematic ? "Through its camera (C)" : "Free camera (C)"))
    {
        m_throughCinematic = !m_throughCinematic;
        if (!m_throughCinematic)
        {
            ViewFrom(player.Picture());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Hide (H)"))
    {
        m_hidden = true;
    }
    if (!m_status.empty() && m_statusAge < 6.0f)
    {
        ImGui::SameLine();
        ImGui::TextColored({0.6f, 0.9f, 0.6f, 1.0f}, "%s", m_status.c_str());
    }

    // The transport.
    if (ImGui::Button("|<"))
    {
        Seek(context, 0.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("<"))
    {
        player.SetPaused(true);
        Seek(context, player.Time() - kFrame);
    }
    ImGui::SameLine();
    const bool playing = player.GetState() == CinematicPlayer::State::Playing;
    if (ImGui::Button(playing ? "Pause" : "Play", {52.0f, 0.0f}))
    {
        PlayPause(context);
    }
    ImGui::SameLine();
    if (ImGui::Button(">"))
    {
        player.SetPaused(true);
        Seek(context, player.Time() + kFrame);
    }
    ImGui::SameLine();
    if (ImGui::Button(">|"))
    {
        Seek(context, cinematic.duration);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &m_loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    static const float kSpeeds[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    char speed[16];
    std::snprintf(speed, sizeof(speed), "%gx", player.Speed());
    if (ImGui::BeginCombo("##speed", speed))
    {
        for (const float s : kSpeeds)
        {
            std::snprintf(speed, sizeof(speed), "%gx", s);
            if (ImGui::Selectable(speed, s == player.Speed()))
            {
                player.SetSpeed(s);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("%6.2f / %.2f s  frame %d", player.Time(), cinematic.duration, static_cast<int>(std::round(player.Time() / kFrame)));
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &m_snap);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(62.0f);
    static const float kSteps[] = {kFrame, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
    char step[16];
    std::snprintf(step, sizeof(step), m_snapStep == kFrame ? "frame" : "%g s", m_snapStep);
    if (ImGui::BeginCombo("##step", step))
    {
        for (const float s : kSteps)
        {
            std::snprintf(step, sizeof(step), s == kFrame ? "frame" : "%g s", s);
            if (ImGui::Selectable(step, s == m_snapStep))
            {
                m_snapStep = s;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &m_follow);
    ImGui::SameLine();
    if (ImGui::Button("Fit"))
    {
        m_fitted = false;
    }
    const std::vector<std::string> unbound = Unbound(context);
    if (!unbound.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.55f, 0.35f, 1.0f}, "%zu unbound", unbound.size());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Not supplied here, so taken as the world's origin:\n%s", Joined(unbound).c_str());
        }
    }
}

void CinematicEditor::DrawAddMenu(Context& context)
{
    if (!ImGui::BeginPopup("##add"))
    {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
    }
    Cinematic& cinematic = Edited(context);
    const float at = context.player->Time();
    if (ImGui::MenuItem("Camera, from the view"))
    {
        CameraTrack camera;
        camera.name = Unused("camera", cinematic.cameras, [](const CameraTrack& c) { return c.name; });
        CameraKey key;
        KeyFromView(context, key, "", at);
        key.time = at;
        camera.keys.push_back(key);
        cinematic.cameras.push_back(camera);
        m_selected = {Pick::CameraKey, static_cast<int>(cinematic.cameras.size()) - 1, 0};
        Changed(context);
    }
    if (ImGui::MenuItem("Shot: cut to a camera"))
    {
        AddAt(context, {"", Pick::Shot, -1}, at);
    }
    if (ImGui::BeginMenu("Actor"))
    {
        const auto add = [&](const std::string& model, const std::string& bind)
        {
            ActorDef actor;
            actor.name = Unused(bind.empty() ? model : bind, cinematic.actors, [](const ActorDef& a) { return a.name; });
            actor.model = model;
            actor.bind = bind;
            cinematic.actors.push_back(actor);
            m_selected = {Pick::Actor, static_cast<int>(cinematic.actors.size()) - 1, -1};
            m_needsReplay = true;
            Changed(context);
        };
        ImGui::TextDisabled("Something the site already has:");
        for (const char* bind : {"site_shuttle", "site_crawler"})
        {
            if (ImGui::MenuItem(bind))
            {
                add("", bind);
            }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("A model brought for it"))
        {
            for (const std::string& model : ListModels())
            {
                if (ImGui::MenuItem(model.c_str()))
                {
                    add(model, "");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Sound"))
    {
        AddAt(context, {"", Pick::Sound, -1}, at);
    }
    if (ImGui::MenuItem("Marker"))
    {
        AddAt(context, {"", Pick::Marker, -1}, at);
    }
    if (ImGui::MenuItem("Title card"))
    {
        AddAt(context, {"", Pick::Text, -1}, at);
    }
    if (ImGui::MenuItem("Caption"))
    {
        AddAt(context, {"", Pick::Text, -1}, at);
        if (m_selected.kind == Pick::Text && InRange(m_selected.item, cinematic.texts.size()))
        {
            TextItem& text = cinematic.texts[static_cast<size_t>(m_selected.item)];
            text.style = "caption";
            text.lines = {"..."};
            text.typeSpeed = 0.0f;
            Changed(context);
        }
    }
    if (ImGui::MenuItem("Particles"))
    {
        AddAt(context, {"", Pick::Particle, -1}, at);
    }
    if (ImGui::MenuItem("Light track"))
    {
        LightTrack light;
        light.light = Unused("light", cinematic.lights, [](const LightTrack& l) { return l.light; });
        cinematic.lights.push_back(light);
        m_selected = {Pick::Light, static_cast<int>(cinematic.lights.size()) - 1, -1};
        Changed(context);
    }
    if (ImGui::BeginMenu("Key"))
    {
        for (int which = 0; which < kFloatLists; ++which)
        {
            if (ImGui::MenuItem(kFloatNames[which]))
            {
                AddAt(context, {"", Pick::Float, which}, at);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

// --- The timeline -------------------------------------------------------------------------------------

std::vector<CinematicEditor::Row> CinematicEditor::Rows(const Cinematic& cinematic) const
{
    std::vector<Row> rows;
    rows.push_back({"Shots", Pick::Shot, -1});
    for (size_t i = 0; i < cinematic.cameras.size(); ++i)
    {
        rows.push_back({cinematic.cameras[i].name, Pick::Camera, static_cast<int>(i)});
    }
    for (size_t i = 0; i < cinematic.actors.size(); ++i)
    {
        rows.push_back({cinematic.actors[i].name, Pick::Actor, static_cast<int>(i)});
    }
    rows.push_back({"Sounds", Pick::Sound, -1});
    rows.push_back({"Markers", Pick::Marker, -1});
    rows.push_back({"Words", Pick::Text, -1});
    rows.push_back({"Particles", Pick::Particle, -1});
    for (size_t i = 0; i < cinematic.lights.size(); ++i)
    {
        rows.push_back({cinematic.lights[i].light, Pick::Light, static_cast<int>(i)});
    }
    for (int which = 0; which < kFloatLists; ++which)
    {
        rows.push_back({kFloatNames[which], Pick::Float, which});
    }
    return rows;
}

std::vector<CinematicEditor::Item> CinematicEditor::ItemsOf(const Row& row, const Cinematic& cinematic) const
{
    std::vector<Item> items;
    const auto point = [&](float time, Selection select, bool key, ImU32 colour, std::string label)
    { items.push_back({time, -1.0f, select, key, colour, std::move(label)}); };
    const auto span = [&](float time, float end, Selection select, ImU32 colour, std::string label)
    { items.push_back({time, end, select, false, colour, std::move(label)}); };
    switch (row.kind)
    {
    case Pick::Shot:
        for (size_t i = 0; i < cinematic.shots.size(); ++i)
        {
            const float end = i + 1 < cinematic.shots.size() ? cinematic.shots[i + 1].time : cinematic.duration;
            span(cinematic.shots[i].time, end, {Pick::Shot, -1, static_cast<int>(i)}, kShotColour, cinematic.shots[i].camera);
        }
        break;
    case Pick::Camera:
    {
        const CameraTrack& camera = cinematic.cameras[static_cast<size_t>(row.track)];
        for (size_t k = 0; k < camera.keys.size(); ++k)
        {
            point(camera.keys[k].time, {Pick::CameraKey, row.track, static_cast<int>(k)}, true, kCameraColour, {});
        }
        break;
    }
    case Pick::Actor:
    {
        const std::string& actor = cinematic.actors[static_cast<size_t>(row.track)].name;
        for (size_t i = 0; i < cinematic.paths.size(); ++i)
        {
            if (cinematic.paths[i].actor == actor)
            {
                span(cinematic.paths[i].start, cinematic.paths[i].end, {Pick::Path, -1, static_cast<int>(i)}, kPathColour, "along " + cinematic.paths[i].path);
            }
        }
        for (size_t t = 0; t < cinematic.actorTracks.size(); ++t)
        {
            if (cinematic.actorTracks[t].actor != actor)
            {
                continue;
            }
            for (size_t k = 0; k < cinematic.actorTracks[t].keys.size(); ++k)
            {
                point(cinematic.actorTracks[t].keys[k].time, {Pick::ActorKey, static_cast<int>(t), static_cast<int>(k)}, true, kActorColour, {});
            }
        }
        for (size_t i = 0; i < cinematic.clips.size(); ++i)
        {
            if (cinematic.clips[i].actor == actor)
            {
                point(cinematic.clips[i].time, {Pick::Clip, -1, static_cast<int>(i)}, false, kClipColour, cinematic.clips[i].clip);
            }
        }
        break;
    }
    case Pick::Sound:
        for (size_t i = 0; i < cinematic.sounds.size(); ++i)
        {
            const std::string& sound = cinematic.sounds[i].sound;
            const size_t slash = sound.find_last_of('/');
            point(cinematic.sounds[i].time, {Pick::Sound, -1, static_cast<int>(i)}, false, kSoundColour,
                  slash == std::string::npos ? sound : sound.substr(slash + 1));
        }
        break;
    case Pick::Marker:
        for (size_t i = 0; i < cinematic.markers.size(); ++i)
        {
            const pred::Marker& marker = cinematic.markers[i];
            point(marker.time, {Pick::Marker, -1, static_cast<int>(i)}, false, kMarkerColour, marker.name + (marker.value.empty() ? "" : " " + marker.value));
        }
        break;
    case Pick::Text:
        for (size_t i = 0; i < cinematic.texts.size(); ++i)
        {
            const TextItem& text = cinematic.texts[i];
            span(text.time, text.time + text.duration, {Pick::Text, -1, static_cast<int>(i)}, kTextColour, text.lines.empty() ? text.style : text.lines.front());
        }
        break;
    case Pick::Particle:
        for (size_t i = 0; i < cinematic.particles.size(); ++i)
        {
            const ParticleEvent& particle = cinematic.particles[i];
            span(particle.time, particle.time + std::max(particle.duration, 0.1f), {Pick::Particle, -1, static_cast<int>(i)}, kParticleColour,
                 particle.effect + (particle.at.empty() ? "" : " at " + particle.at));
        }
        break;
    case Pick::Light:
    {
        const LightTrack& light = cinematic.lights[static_cast<size_t>(row.track)];
        for (size_t k = 0; k < light.intensity.size(); ++k)
        {
            point(light.intensity[k].time, {Pick::LightKey, row.track, static_cast<int>(k)}, true, kLightColour, {});
        }
        break;
    }
    case Pick::Float:
    {
        const std::vector<FloatKey>& keys = FloatList(cinematic, row.track);
        for (size_t k = 0; k < keys.size(); ++k)
        {
            point(keys[k].time, {Pick::Float, row.track, static_cast<int>(k)}, true, kFloatColour, {});
        }
        break;
    }
    default: break;
    }
    return items;
}

namespace
{

// Whether a row is the one the selection is on, to show it picked out.
template <typename Row, typename Selection, typename Pick>
bool OnRow(const Cinematic& cinematic, const Row& row, const Selection& select)
{
    const auto actorRow = [&](const std::string& actor)
    { return row.kind == Pick::Actor && cinematic.actors[static_cast<size_t>(row.track)].name == actor; };
    switch (select.kind)
    {
    case Pick::Camera:
    case Pick::CameraKey: return row.kind == Pick::Camera && row.track == select.track;
    case Pick::Actor: return row.kind == Pick::Actor && row.track == select.track;
    case Pick::ActorKey: return actorRow(cinematic.actorTracks[static_cast<size_t>(select.track)].actor);
    case Pick::Path: return actorRow(cinematic.paths[static_cast<size_t>(select.item)].actor);
    case Pick::Clip: return actorRow(cinematic.clips[static_cast<size_t>(select.item)].actor);
    case Pick::Light:
    case Pick::LightKey: return row.kind == Pick::Light && row.track == select.track;
    case Pick::Float: return row.kind == Pick::Float && row.track == select.track;
    case Pick::None: return false;
    default: return row.kind == select.kind;
    }
}

} // namespace

void CinematicEditor::DrawTimeline(Context& context)
{
    Cinematic& cinematic = Edited(context);
    const std::vector<Row> rows = Rows(cinematic);
    const ImGuiIO& io = ImGui::GetIO();
    const CinematicSampler sampler = context.player->Sampler();
    const float playhead = context.player->Time();
    const bool playing = context.player->GetState() == CinematicPlayer::State::Playing;

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, kLabelWidth + 80.0f);
    const float trackLeft = start.x + kLabelWidth;
    const float trackRight = start.x + width - ImGui::GetStyle().ScrollbarSize;
    const float trackWidth = trackRight - trackLeft;
    if (!m_fitted)
    {
        m_pixelsPerSecond = std::clamp(trackWidth / std::max(cinematic.duration, 1.0f) * 0.98f, 4.0f, 1200.0f);
        m_scroll = 0.0f;
        m_fitted = true;
    }
    const auto X = [&](float t) { return trackLeft + (t - m_scroll) * m_pixelsPerSecond; };
    const auto T = [&](float x) { return m_scroll + (x - trackLeft) / m_pixelsPerSecond; };
    const auto zoomAt = [&](float x, float wheel)
    {
        const float at = T(x);
        m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * std::pow(1.2f, wheel), 4.0f, 1200.0f);
        m_scroll = std::max(at - (x - trackLeft) / m_pixelsPerSecond, 0.0f);
    };
    // Kept in view while it plays.
    if (m_follow && playing && !m_scrubbing && (X(playhead) > trackRight - 16.0f || X(playhead) < trackLeft))
    {
        m_scroll = std::max(playhead - trackWidth * 0.1f / m_pixelsPerSecond, 0.0f);
    }

    // --- The ruler: seconds, and the playhead, dragged to scrub.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##ruler", {width, kRulerHeight});
    const bool rulerHovered = ImGui::IsItemHovered();
    const bool rulerHeld = ImGui::IsItemActive();
    draw->AddRectFilled(start, {start.x + width, start.y + kRulerHeight}, IM_COL32(24, 26, 30, 255));
    float step = 60.0f;
    for (const float s : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f})
    {
        if (s * m_pixelsPerSecond >= 56.0f)
        {
            step = s;
            break;
        }
    }
    draw->PushClipRect({trackLeft, start.y}, {trackRight, start.y + kRulerHeight}, true);
    for (float t = std::floor(m_scroll / step) * step; X(t) < trackRight; t += step)
    {
        const float x = X(t);
        draw->AddLine({x, start.y + 9.0f}, {x, start.y + kRulerHeight}, IM_COL32(130, 134, 142, 255));
        for (int minor = 1; minor < 5; ++minor)
        {
            const float xm = X(t + step * static_cast<float>(minor) / 5.0f);
            draw->AddLine({xm, start.y + 16.0f}, {xm, start.y + kRulerHeight}, IM_COL32(74, 78, 86, 255));
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%g", std::round(t * 100.0f) / 100.0f);
        draw->AddText({x + 3.0f, start.y + 1.0f}, IM_COL32(176, 180, 188, 255), label);
    }
    draw->AddRectFilled({std::max(X(cinematic.duration), trackLeft), start.y}, {trackRight, start.y + kRulerHeight}, IM_COL32(0, 0, 0, 110));
    const float headX = X(playhead);
    draw->AddTriangleFilled({headX - 6.0f, start.y + 8.0f}, {headX + 6.0f, start.y + 8.0f}, {headX, start.y + kRulerHeight}, kPlayheadColour);
    draw->PopClipRect();
    char now[32];
    std::snprintf(now, sizeof(now), "%.2f s", playhead);
    draw->AddText({start.x + 6.0f, start.y + 3.0f}, IM_COL32(236, 238, 242, 255), now);
    if (rulerHeld && io.MousePos.x >= trackLeft - 6.0f)
    {
        if (!m_scrubbing)
        {
            m_scrubbing = true;
            context.player->SetPaused(true);
        }
        Seek(context, std::max(Snapped(T(io.MousePos.x), -1.0e6f), 0.0f));
    }
    else
    {
        m_scrubbing = false;
    }
    if (rulerHovered && io.MouseWheel != 0.0f)
    {
        zoomAt(io.MousePos.x, io.MouseWheel);
    }

    // --- The rows.
    const float rowsHeight = std::max(ImGui::GetContentRegionAvail().y, kRowHeight);
    ImGui::BeginChild("##rows", {width, rowsHeight}, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* rowsDraw = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float visibleTop = ImGui::GetWindowPos().y;
    const float visibleBottom = visibleTop + ImGui::GetWindowSize().y;
    const float contentHeight = std::max(static_cast<float>(rows.size()) * kRowHeight, rowsHeight - 4.0f);
    ImGui::InvisibleButton("##canvas", {width - ImGui::GetStyle().ScrollbarSize, contentHeight}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                                                                   ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    for (size_t r = 0; r < rows.size(); ++r)
    {
        const Row& row = rows[r];
        const float y0 = top.y + static_cast<float>(r) * kRowHeight;
        const float y1 = y0 + kRowHeight;
        if (y1 < visibleTop || y0 > visibleBottom)
        {
            continue;
        }
        const bool picked = OnRow<Row, Selection, Pick>(cinematic, row, m_selected);
        rowsDraw->AddRectFilled({trackLeft, y0}, {trackRight, y1}, r % 2 == 0 ? IM_COL32(32, 34, 38, 255) : IM_COL32(28, 30, 34, 255));
        rowsDraw->AddRectFilled({top.x, y0}, {trackLeft - 2.0f, y1}, picked ? IM_COL32(56, 66, 92, 255) : IM_COL32(40, 42, 48, 255));
        ImU32 labelColour = IM_COL32(200, 204, 212, 255);
        switch (row.kind)
        {
        case Pick::Camera: labelColour = kCameraColour; break;
        case Pick::Actor: labelColour = kActorColour; break;
        case Pick::Light: labelColour = kLightColour; break;
        default: break;
        }
        rowsDraw->PushClipRect({top.x, y0}, {trackLeft - 4.0f, y1}, true);
        rowsDraw->AddText({top.x + 6.0f, y0 + 4.0f}, labelColour, row.label.c_str());
        rowsDraw->PopClipRect();

        rowsDraw->PushClipRect({trackLeft, y0}, {trackRight, y1}, true);
        // Past the end, dimmed.
        rowsDraw->AddRectFilled({std::max(X(cinematic.duration), trackLeft), y0}, {trackRight, y1}, IM_COL32(0, 0, 0, 90));
        // A camera's row shows when it is the one in the picture.
        if (row.kind == Pick::Camera)
        {
            const std::string& name = cinematic.cameras[static_cast<size_t>(row.track)].name;
            for (size_t i = 0; i < cinematic.shots.size(); ++i)
            {
                if (cinematic.shots[i].camera == name)
                {
                    const float end = i + 1 < cinematic.shots.size() ? cinematic.shots[i + 1].time : cinematic.duration;
                    rowsDraw->AddRectFilled({X(cinematic.shots[i].time), y0 + 2.0f}, {X(end), y1 - 2.0f}, Faded(kCameraColour, 34));
                }
            }
        }
        // A value's row draws the value.
        if (row.kind == Pick::Float || row.kind == Pick::Light)
        {
            const std::vector<FloatKey>& keys =
                row.kind == Pick::Float ? FloatList(cinematic, row.track) : cinematic.lights[static_cast<size_t>(row.track)].intensity;
            const float none = row.kind == Pick::Float ? kFloatNone[row.track] : 1.0f;
            if (!keys.empty())
            {
                float low = none;
                float high = none;
                for (const FloatKey& key : keys)
                {
                    low = std::min(low, key.value);
                    high = std::max(high, key.value);
                }
                const float range = std::max(high - low, 1.0e-3f);
                ImVec2 last{};
                for (float x = trackLeft; x <= trackRight; x += 3.0f)
                {
                    const float v = (SampleFloat(keys, T(x), none) - low) / range;
                    const ImVec2 p{x, y1 - 3.0f - v * (kRowHeight - 6.0f)};
                    if (x > trackLeft)
                    {
                        rowsDraw->AddLine(last, p, Faded(row.kind == Pick::Float ? kFloatColour : kLightColour, 120));
                    }
                    last = p;
                }
            }
        }
        const std::vector<Item> items = ItemsOf(row, cinematic);
        for (const Item& item : items)
        {
            if (item.end < 0.0f)
            {
                continue;
            }
            const float x0 = X(item.time);
            const float x1 = std::max(X(item.end), x0 + 3.0f);
            const bool selected = item.select == m_selected;
            rowsDraw->AddRectFilled({x0, y0 + 3.0f}, {x1, y1 - 3.0f}, Faded(item.colour, selected ? 235 : 150), 3.0f);
            if (selected)
            {
                rowsDraw->AddRect({x0, y0 + 3.0f}, {x1, y1 - 3.0f}, IM_COL32(255, 255, 255, 255), 3.0f, 0, 2.0f);
            }
            if (item.select.kind == Pick::Shot && item.select.item > 0 && cinematic.shots[static_cast<size_t>(item.select.item)].blend > 0.0f)
            {
                // A blend in: the wedge it comes in over.
                const float blend = X(item.time + cinematic.shots[static_cast<size_t>(item.select.item)].blend);
                rowsDraw->AddTriangleFilled({x0, y1 - 3.0f}, {blend, y0 + 3.0f}, {x0, y0 + 3.0f}, IM_COL32(20, 22, 26, 150));
            }
            rowsDraw->PushClipRect({x0, y0}, {x1 - 2.0f, y1}, true);
            rowsDraw->AddText({x0 + 5.0f, y0 + 4.0f}, IM_COL32(12, 14, 18, 255), item.label.c_str());
            rowsDraw->PopClipRect();
        }
        std::vector<float> starts;
        for (const Item& item : items)
        {
            starts.push_back(X(item.time));
        }
        std::sort(starts.begin(), starts.end());
        for (const Item& item : items)
        {
            if (item.end >= 0.0f)
            {
                continue;
            }
            const float x = X(item.time);
            const float cy = (y0 + y1) * 0.5f;
            const bool selected = item.select == m_selected;
            // Its label has until the next thing on the row, unless it is the one selected.
            const auto next = std::upper_bound(starts.begin(), starts.end(), x + 0.5f);
            const float room = selected || next == starts.end() ? trackRight : std::min(*next - 3.0f, trackRight);
            if (item.key)
            {
                const float s = selected ? 7.0f : 5.5f;
                rowsDraw->AddQuadFilled({x, cy - s}, {x + s, cy}, {x, cy + s}, {x - s, cy}, item.colour);
                rowsDraw->AddQuad({x, cy - s}, {x + s, cy}, {x, cy + s}, {x - s, cy}, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), selected ? 2.0f : 1.0f);
            }
            else
            {
                rowsDraw->AddLine({x, y0 + 2.0f}, {x, y1 - 2.0f}, item.colour, selected ? 3.0f : 2.0f);
                rowsDraw->AddTriangleFilled({x, y0 + 2.0f}, {x + 9.0f, y0 + 6.5f}, {x, y0 + 11.0f}, item.colour);
                if (room > x + 16.0f)
                {
                    rowsDraw->PushClipRect({x, y0}, {room, y1}, true);
                    if (selected)
                    {
                        // Over whatever it covers, so it can be read.
                        const ImVec2 size = ImGui::CalcTextSize(item.label.c_str());
                        rowsDraw->AddRectFilled({x + 10.0f, y0 + 3.0f}, {x + 13.0f + size.x, y1 - 3.0f}, IM_COL32(20, 22, 26, 230));
                    }
                    rowsDraw->AddText({x + 11.0f, y0 + 4.0f}, selected ? IM_COL32(255, 255, 255, 255) : Faded(item.colour, 230), item.label.c_str());
                    rowsDraw->PopClipRect();
                }
            }
        }
        rowsDraw->PopClipRect();
    }
    // The playhead, down through every row.
    rowsDraw->PushClipRect({trackLeft, visibleTop}, {trackRight, visibleBottom}, true);
    rowsDraw->AddLine({headX, visibleTop}, {headX, visibleBottom}, kPlayheadColour, 1.5f);
    rowsDraw->PopClipRect();

    // --- Pointing at it.
    int hoverRow = -1;
    if (hovered && mouse.y >= top.y)
    {
        hoverRow = static_cast<int>((mouse.y - top.y) / kRowHeight);
        if (hoverRow >= static_cast<int>(rows.size()))
        {
            hoverRow = -1;
        }
    }
    Selection hit;
    bool hitAny = false;
    if (hoverRow >= 0 && mouse.x >= trackLeft - 6.0f)
    {
        // A key or a flag nearest the pointer first, since they sit on top of the spans.
        const std::vector<Item> items = ItemsOf(rows[static_cast<size_t>(hoverRow)], cinematic);
        float best = 7.0f;
        for (const Item& item : items)
        {
            if (item.end < 0.0f && std::abs(mouse.x - X(item.time)) < best)
            {
                best = std::abs(mouse.x - X(item.time));
                hit = item.select;
                hitAny = true;
            }
        }
        if (!hitAny)
        {
            for (auto it = items.rbegin(); it != items.rend(); ++it)
            {
                if (it->end >= 0.0f && mouse.x >= X(it->time) - 3.0f && mouse.x <= std::max(X(it->end), X(it->time) + 3.0f) + 3.0f)
                {
                    hit = it->select;
                    hitAny = true;
                    break;
                }
            }
        }
    }
    const auto rowSelection = [&](const Row& row) -> Selection
    {
        switch (row.kind)
        {
        case Pick::Camera:
        case Pick::Actor:
        case Pick::Light:
        case Pick::Float: return {row.kind, row.track, -1};
        default: return {row.kind, -1, -1};
        }
    };
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (hitAny)
        {
            m_selected = hit;
            // Alt dragging leaves a copy behind.
            if (io.KeyAlt)
            {
                Duplicate(context, TimeOf(cinematic, hit));
            }
            m_dragging = true;
            m_grab = T(mouse.x) - TimeOf(cinematic, m_selected);
        }
        else
        {
            m_selected = rowSelection(rows[static_cast<size_t>(hoverRow)]);
        }
    }
    if (hovered && hoverRow >= 0 && !hitAny && mouse.x >= trackLeft && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        AddAt(context, rows[static_cast<size_t>(hoverRow)], std::max(Snapped(T(mouse.x), playhead), 0.0f));
    }
    if (m_dragging)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && Valid(cinematic, m_selected))
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            {
                const float t = std::max(Snapped(T(mouse.x) - m_grab, playhead), 0.0f);
                if (std::abs(t - TimeOf(cinematic, m_selected)) > 1.0e-5f)
                {
                    SetTime(cinematic, m_selected, t);
                    Changed(context);
                }
            }
        }
        else
        {
            m_dragging = false;
        }
    }
    if (hovered && hoverRow >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        m_selected = hitAny ? hit : rowSelection(rows[static_cast<size_t>(hoverRow)]);
        m_menuRow = hoverRow;
        m_menuTime = std::max(Snapped(T(mouse.x), playhead), 0.0f);
        ImGui::OpenPopup("##rowmenu");
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
    {
        m_scroll = std::max(m_scroll - io.MouseDelta.x / m_pixelsPerSecond, 0.0f);
    }
    if (ImGui::IsWindowHovered())
    {
        if (io.KeyCtrl && io.MouseWheel != 0.0f)
        {
            zoomAt(mouse.x, io.MouseWheel);
        }
        else if (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f))
        {
            const float amount = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
            m_scroll = std::max(m_scroll - amount * 80.0f / m_pixelsPerSecond, 0.0f);
        }
        else if (io.MouseWheel != 0.0f)
        {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * kRowHeight * 2.0f);
        }
    }
    DrawRowMenu(context);
    ImGui::EndChild();
}

void CinematicEditor::DrawRowMenu(Context& context)
{
    if (!ImGui::BeginPopup("##rowmenu"))
    {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
    }
    Cinematic& cinematic = Edited(context);
    const std::vector<Row> rows = Rows(cinematic);
    const bool onItem = m_selected.item >= 0 || m_selected.kind == Pick::Path || m_selected.kind == Pick::Clip;
    if (onItem && Valid(cinematic, m_selected))
    {
        if (ImGui::MenuItem("Duplicate at playhead", "Ctrl+D"))
        {
            Duplicate(context, context.player->Time());
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            Copy(cinematic);
        }
        if (ImGui::MenuItem("Paste values", "Ctrl+V", false, m_copied.index() != 0))
        {
            Paste(context, context.player->Time());
        }
        if (ImGui::MenuItem("Go to it"))
        {
            Seek(context, TimeOf(cinematic, m_selected));
        }
        // Easing, where it has one.
        Ease* ease = nullptr;
        switch (m_selected.kind)
        {
        case Pick::CameraKey: ease = &cinematic.cameras[static_cast<size_t>(m_selected.track)].keys[static_cast<size_t>(m_selected.item)].ease; break;
        case Pick::ActorKey: ease = &cinematic.actorTracks[static_cast<size_t>(m_selected.track)].keys[static_cast<size_t>(m_selected.item)].ease; break;
        case Pick::LightKey: ease = &cinematic.lights[static_cast<size_t>(m_selected.track)].intensity[static_cast<size_t>(m_selected.item)].ease; break;
        case Pick::Float: ease = &FloatList(cinematic, m_selected.track)[static_cast<size_t>(m_selected.item)].ease; break;
        case Pick::Shot: ease = &cinematic.shots[static_cast<size_t>(m_selected.item)].ease; break;
        case Pick::Path: ease = &cinematic.paths[static_cast<size_t>(m_selected.item)].ease; break;
        default: break;
        }
        if (ease != nullptr && ImGui::BeginMenu("Ease"))
        {
            for (int kind = 0; kind < 5; ++kind)
            {
                if (ImGui::MenuItem(kEaseNames[kind], nullptr, static_cast<int>(ease->kind) == kind))
                {
                    ease->kind = static_cast<Ease::Kind>(kind);
                    Changed(context);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete", "Del"))
        {
            Delete(context);
        }
        ImGui::Separator();
    }
    if (InRange(m_menuRow, rows.size()))
    {
        const Row& row = rows[static_cast<size_t>(m_menuRow)];
        char label[64];
        std::snprintf(label, sizeof(label), "Add here (%.2f s)", m_menuTime);
        if (ImGui::MenuItem(label))
        {
            AddAt(context, row, m_menuTime);
        }
        if (m_copied.index() != 0 && ImGui::MenuItem("Paste here"))
        {
            Paste(context, m_menuTime);
        }
        if ((row.kind == Pick::Camera || row.kind == Pick::Actor || row.kind == Pick::Light) && ImGui::MenuItem("Delete this track"))
        {
            m_selected = {row.kind, row.track, -1};
            Delete(context);
        }
    }
    ImGui::EndPopup();
}

// --- What is selected, and doing things to it ------------------------------------------------------------

bool CinematicEditor::Valid(const Cinematic& cinematic, const Selection& select) const
{
    const auto rowOrItem = [&](size_t size) { return select.item == -1 || InRange(select.item, size); };
    switch (select.kind)
    {
    case Pick::None: return true;
    case Pick::Camera: return InRange(select.track, cinematic.cameras.size());
    case Pick::CameraKey:
        return InRange(select.track, cinematic.cameras.size()) && InRange(select.item, cinematic.cameras[static_cast<size_t>(select.track)].keys.size());
    case Pick::Shot: return rowOrItem(cinematic.shots.size());
    case Pick::Actor: return InRange(select.track, cinematic.actors.size());
    case Pick::ActorKey:
        return InRange(select.track, cinematic.actorTracks.size()) &&
               InRange(select.item, cinematic.actorTracks[static_cast<size_t>(select.track)].keys.size());
    case Pick::Path: return InRange(select.item, cinematic.paths.size());
    case Pick::Clip: return InRange(select.item, cinematic.clips.size());
    case Pick::Sound: return rowOrItem(cinematic.sounds.size());
    case Pick::Marker: return rowOrItem(cinematic.markers.size());
    case Pick::Text: return rowOrItem(cinematic.texts.size());
    case Pick::Particle: return rowOrItem(cinematic.particles.size());
    case Pick::Light: return InRange(select.track, cinematic.lights.size());
    case Pick::LightKey:
        return InRange(select.track, cinematic.lights.size()) &&
               InRange(select.item, cinematic.lights[static_cast<size_t>(select.track)].intensity.size());
    case Pick::Float: return select.track >= 0 && select.track < kFloatLists && rowOrItem(FloatList(cinematic, select.track).size());
    }
    return false;
}

float CinematicEditor::TimeOf(const Cinematic& cinematic, const Selection& select) const
{
    if (!Valid(cinematic, select) || (select.item < 0 && select.kind != Pick::Path && select.kind != Pick::Clip))
    {
        return 0.0f;
    }
    const auto item = static_cast<size_t>(select.item);
    const auto track = static_cast<size_t>(std::max(select.track, 0));
    switch (select.kind)
    {
    case Pick::CameraKey: return cinematic.cameras[track].keys[item].time;
    case Pick::Shot: return cinematic.shots[item].time;
    case Pick::ActorKey: return cinematic.actorTracks[track].keys[item].time;
    case Pick::Path: return cinematic.paths[item].start;
    case Pick::Clip: return cinematic.clips[item].time;
    case Pick::Sound: return cinematic.sounds[item].time;
    case Pick::Marker: return cinematic.markers[item].time;
    case Pick::Text: return cinematic.texts[item].time;
    case Pick::Particle: return cinematic.particles[item].time;
    case Pick::LightKey: return cinematic.lights[track].intensity[item].time;
    case Pick::Float: return FloatList(cinematic, select.track)[item].time;
    default: return 0.0f;
    }
}

void CinematicEditor::SetTime(Cinematic& cinematic, const Selection& select, float time)
{
    if (!Valid(cinematic, select) || select.item < 0)
    {
        return;
    }
    const auto item = static_cast<size_t>(select.item);
    const auto track = static_cast<size_t>(std::max(select.track, 0));
    switch (select.kind)
    {
    case Pick::CameraKey: cinematic.cameras[track].keys[item].time = time; break;
    case Pick::Shot: cinematic.shots[item].time = time; break;
    case Pick::ActorKey: cinematic.actorTracks[track].keys[item].time = time; break;
    case Pick::Path:
    {
        // The whole drive moves, taking as long as it did.
        PathFollow& follow = cinematic.paths[item];
        follow.end += time - follow.start;
        follow.start = time;
        break;
    }
    case Pick::Clip: cinematic.clips[item].time = time; break;
    case Pick::Sound: cinematic.sounds[item].time = time; break;
    case Pick::Marker: cinematic.markers[item].time = time; break;
    case Pick::Text: cinematic.texts[item].time = time; break;
    case Pick::Particle: cinematic.particles[item].time = time; break;
    case Pick::LightKey: cinematic.lights[track].intensity[item].time = time; break;
    case Pick::Float: FloatList(cinematic, select.track)[item].time = time; break;
    default: break;
    }
}

void CinematicEditor::Resort(Cinematic& cinematic)
{
    if (!Valid(cinematic, m_selected) || m_selected.item < 0)
    {
        return;
    }
    const auto track = static_cast<size_t>(std::max(m_selected.track, 0));
    int& item = m_selected.item;
    switch (m_selected.kind)
    {
    case Pick::CameraKey: item = SortByTime(cinematic.cameras[track].keys, item); break;
    case Pick::Shot: item = SortByTime(cinematic.shots, item); break;
    case Pick::ActorKey: item = SortByTime(cinematic.actorTracks[track].keys, item); break;
    case Pick::Clip: item = SortByTime(cinematic.clips, item); break;
    case Pick::Sound: item = SortByTime(cinematic.sounds, item); break;
    case Pick::Marker: item = SortByTime(cinematic.markers, item); break;
    case Pick::Text: item = SortByTime(cinematic.texts, item); break;
    case Pick::Particle: item = SortByTime(cinematic.particles, item); break;
    case Pick::LightKey: item = SortByTime(cinematic.lights[track].intensity, item); break;
    case Pick::Float: item = SortByTime(FloatList(cinematic, m_selected.track), item); break;
    default: break;
    }
}

void CinematicEditor::Delete(Context& context)
{
    Cinematic& cinematic = Edited(context);
    if (!Valid(cinematic, m_selected))
    {
        return;
    }
    const auto item = static_cast<size_t>(std::max(m_selected.item, 0));
    const auto track = static_cast<size_t>(std::max(m_selected.track, 0));
    const auto erase = [](auto& list, size_t index) { list.erase(list.begin() + static_cast<std::ptrdiff_t>(index)); };
    const bool one = m_selected.item >= 0;
    switch (m_selected.kind)
    {
    case Pick::CameraKey:
        erase(cinematic.cameras[track].keys, item);
        m_selected = {Pick::Camera, m_selected.track, -1};
        break;
    case Pick::Camera:
    {
        // And the cuts to it.
        const std::string name = cinematic.cameras[track].name;
        erase(cinematic.cameras, track);
        std::erase_if(cinematic.shots, [&](const Shot& shot) { return shot.camera == name; });
        m_selected = {};
        break;
    }
    case Pick::Actor:
    {
        // And everything that moves it.
        const std::string name = cinematic.actors[track].name;
        erase(cinematic.actors, track);
        std::erase_if(cinematic.actorTracks, [&](const ActorTrack& t) { return t.actor == name; });
        std::erase_if(cinematic.paths, [&](const PathFollow& p) { return p.actor == name; });
        std::erase_if(cinematic.clips, [&](const ClipEvent& c) { return c.actor == name; });
        m_needsReplay = true;
        m_selected = {};
        break;
    }
    case Pick::ActorKey:
    {
        ActorTrack& keys = cinematic.actorTracks[track];
        erase(keys.keys, item);
        const std::string actor = keys.actor;
        if (keys.keys.empty())
        {
            erase(cinematic.actorTracks, track);
        }
        m_selected = {};
        for (size_t a = 0; a < cinematic.actors.size(); ++a)
        {
            if (cinematic.actors[a].name == actor)
            {
                m_selected = {Pick::Actor, static_cast<int>(a), -1};
            }
        }
        break;
    }
    case Pick::Path: erase(cinematic.paths, item), m_selected = {}; break;
    case Pick::Clip: erase(cinematic.clips, item), m_selected = {}; break;
    case Pick::Shot:
        if (one)
        {
            erase(cinematic.shots, item), m_selected.item = -1;
        }
        break;
    case Pick::Sound:
        if (one)
        {
            erase(cinematic.sounds, item), m_selected.item = -1;
        }
        break;
    case Pick::Marker:
        if (one)
        {
            erase(cinematic.markers, item), m_selected.item = -1;
        }
        break;
    case Pick::Text:
        if (one)
        {
            erase(cinematic.texts, item), m_selected.item = -1;
        }
        break;
    case Pick::Particle:
        if (one)
        {
            erase(cinematic.particles, item), m_selected.item = -1;
        }
        break;
    case Pick::Light: erase(cinematic.lights, track), m_selected = {}; break;
    case Pick::LightKey: erase(cinematic.lights[track].intensity, item), m_selected = {Pick::Light, m_selected.track, -1}; break;
    case Pick::Float:
        if (one)
        {
            erase(FloatList(cinematic, m_selected.track), item), m_selected.item = -1;
        }
        break;
    case Pick::None: break;
    }
    Changed(context);
}

void CinematicEditor::Duplicate(Context& context, float at)
{
    Cinematic& cinematic = Edited(context);
    if (!Valid(cinematic, m_selected) || (m_selected.item < 0 && m_selected.kind != Pick::Path && m_selected.kind != Pick::Clip))
    {
        return;
    }
    const auto item = static_cast<size_t>(m_selected.item);
    const auto track = static_cast<size_t>(std::max(m_selected.track, 0));
    // A copy at `at`, selected, in the same list.
    const auto copy = [&](auto& list)
    {
        auto made = list[item];
        made.time = at;
        list.push_back(made);
        m_selected.item = static_cast<int>(list.size()) - 1;
    };
    switch (m_selected.kind)
    {
    case Pick::CameraKey: copy(cinematic.cameras[track].keys); break;
    case Pick::Shot: copy(cinematic.shots); break;
    case Pick::ActorKey: copy(cinematic.actorTracks[track].keys); break;
    case Pick::Clip: copy(cinematic.clips); break;
    case Pick::Sound: copy(cinematic.sounds); break;
    case Pick::Marker: copy(cinematic.markers); break;
    case Pick::Text: copy(cinematic.texts); break;
    case Pick::Particle: copy(cinematic.particles); break;
    case Pick::LightKey: copy(cinematic.lights[track].intensity); break;
    case Pick::Float: copy(FloatList(cinematic, m_selected.track)); break;
    case Pick::Path:
    {
        PathFollow made = cinematic.paths[item];
        made.end += at - made.start;
        made.start = at;
        cinematic.paths.push_back(made);
        m_selected.item = static_cast<int>(cinematic.paths.size()) - 1;
        break;
    }
    default: return;
    }
    Changed(context);
}

void CinematicEditor::Copy(const Cinematic& cinematic)
{
    if (!Valid(cinematic, m_selected))
    {
        return;
    }
    const auto item = static_cast<size_t>(std::max(m_selected.item, 0));
    const auto track = static_cast<size_t>(std::max(m_selected.track, 0));
    const bool one = m_selected.item >= 0;
    switch (m_selected.kind)
    {
    case Pick::CameraKey: m_copied = cinematic.cameras[track].keys[item]; break;
    case Pick::ActorKey: m_copied = cinematic.actorTracks[track].keys[item]; break;
    case Pick::LightKey: m_copied = cinematic.lights[track].intensity[item]; break;
    case Pick::Float:
        if (one)
        {
            m_copied = FloatList(cinematic, m_selected.track)[item];
        }
        break;
    case Pick::Shot:
        if (one)
        {
            m_copied = cinematic.shots[item];
        }
        break;
    case Pick::Path: m_copied = cinematic.paths[item]; break;
    case Pick::Clip: m_copied = cinematic.clips[item]; break;
    case Pick::Sound:
        if (one)
        {
            m_copied = cinematic.sounds[item];
        }
        break;
    case Pick::Marker:
        if (one)
        {
            m_copied = cinematic.markers[item];
        }
        break;
    case Pick::Text:
        if (one)
        {
            m_copied = cinematic.texts[item];
        }
        break;
    case Pick::Particle:
        if (one)
        {
            m_copied = cinematic.particles[item];
        }
        break;
    default: return;
    }
    m_copiedFrom = m_selected;
    m_status = "Copied";
    m_statusAge = 0.0f;
}

void CinematicEditor::Paste(Context& context, float at)
{
    Cinematic& cinematic = Edited(context);
    if (m_copied.index() == 0)
    {
        return;
    }
    const auto item = static_cast<size_t>(std::max(m_selected.item, 0));
    const bool one = Valid(cinematic, m_selected) && m_selected.item >= 0;
    // Onto one of the same kind: its values, at its own time.
    const auto onto = [&](auto& target, const auto& values)
    {
        const float time = target.time;
        target = values;
        target.time = time;
    };
    // Otherwise a new one at `at`, on the track it came from or the one selected.
    const auto insert = [&](auto& list, auto values, Pick kind, int track)
    {
        values.time = at;
        list.push_back(values);
        m_selected = {kind, track, static_cast<int>(list.size()) - 1};
    };
    const int fromTrack = m_copiedFrom.track;
    const auto sameTrack = [&](Pick track, Pick key, size_t size)
    {
        if ((m_selected.kind == track || m_selected.kind == key) && InRange(m_selected.track, size))
        {
            return m_selected.track;
        }
        return InRange(fromTrack, size) ? fromTrack : -1;
    };
    if (const CameraKey* key = std::get_if<CameraKey>(&m_copied))
    {
        if (one && m_selected.kind == Pick::CameraKey)
        {
            onto(cinematic.cameras[static_cast<size_t>(m_selected.track)].keys[item], *key);
        }
        else if (const int track = sameTrack(Pick::Camera, Pick::CameraKey, cinematic.cameras.size()); track >= 0)
        {
            insert(cinematic.cameras[static_cast<size_t>(track)].keys, *key, Pick::CameraKey, track);
        }
    }
    else if (const TransformKey* transform = std::get_if<TransformKey>(&m_copied))
    {
        if (one && m_selected.kind == Pick::ActorKey)
        {
            onto(cinematic.actorTracks[static_cast<size_t>(m_selected.track)].keys[item], *transform);
        }
        else if (InRange(fromTrack, cinematic.actorTracks.size()))
        {
            insert(cinematic.actorTracks[static_cast<size_t>(fromTrack)].keys, *transform, Pick::ActorKey, fromTrack);
        }
    }
    else if (const FloatKey* value = std::get_if<FloatKey>(&m_copied))
    {
        if (one && m_selected.kind == Pick::Float)
        {
            onto(FloatList(cinematic, m_selected.track)[item], *value);
        }
        else if (one && m_selected.kind == Pick::LightKey)
        {
            onto(cinematic.lights[static_cast<size_t>(m_selected.track)].intensity[item], *value);
        }
        else if (m_selected.kind == Pick::Float && m_selected.track >= 0)
        {
            insert(FloatList(cinematic, m_selected.track), *value, Pick::Float, m_selected.track);
        }
        else if (m_copiedFrom.kind == Pick::Float)
        {
            insert(FloatList(cinematic, fromTrack), *value, Pick::Float, fromTrack);
        }
        else if (InRange(fromTrack, cinematic.lights.size()))
        {
            insert(cinematic.lights[static_cast<size_t>(fromTrack)].intensity, *value, Pick::LightKey, fromTrack);
        }
    }
    else if (const Shot* shot = std::get_if<Shot>(&m_copied))
    {
        one && m_selected.kind == Pick::Shot ? onto(cinematic.shots[item], *shot) : insert(cinematic.shots, *shot, Pick::Shot, -1);
    }
    else if (const PathFollow* follow = std::get_if<PathFollow>(&m_copied))
    {
        PathFollow made = *follow;
        made.end += at - made.start;
        made.start = at;
        cinematic.paths.push_back(made);
        m_selected = {Pick::Path, -1, static_cast<int>(cinematic.paths.size()) - 1};
    }
    else if (const ClipEvent* clip = std::get_if<ClipEvent>(&m_copied))
    {
        one && m_selected.kind == Pick::Clip ? onto(cinematic.clips[item], *clip) : insert(cinematic.clips, *clip, Pick::Clip, -1);
    }
    else if (const SoundEvent* sound = std::get_if<SoundEvent>(&m_copied))
    {
        one && m_selected.kind == Pick::Sound ? onto(cinematic.sounds[item], *sound) : insert(cinematic.sounds, *sound, Pick::Sound, -1);
    }
    else if (const pred::Marker* marker = std::get_if<pred::Marker>(&m_copied))
    {
        one && m_selected.kind == Pick::Marker ? onto(cinematic.markers[item], *marker) : insert(cinematic.markers, *marker, Pick::Marker, -1);
    }
    else if (const TextItem* text = std::get_if<TextItem>(&m_copied))
    {
        one && m_selected.kind == Pick::Text ? onto(cinematic.texts[item], *text) : insert(cinematic.texts, *text, Pick::Text, -1);
    }
    else if (const ParticleEvent* particle = std::get_if<ParticleEvent>(&m_copied))
    {
        one && m_selected.kind == Pick::Particle ? onto(cinematic.particles[item], *particle) : insert(cinematic.particles, *particle, Pick::Particle, -1);
    }
    Changed(context);
}

void CinematicEditor::AddAt(Context& context, const Row& row, float time)
{
    Cinematic& cinematic = Edited(context);
    const CinematicSampler sampler = context.player->Sampler();
    switch (row.kind)
    {
    case Pick::Shot:
    {
        Shot shot;
        shot.time = time;
        shot.camera = SelectedCamera(cinematic);
        if (shot.camera.empty() && !cinematic.cameras.empty())
        {
            shot.camera = cinematic.cameras.front().name;
        }
        cinematic.shots.push_back(shot);
        m_selected = {Pick::Shot, -1, static_cast<int>(cinematic.shots.size()) - 1};
        break;
    }
    case Pick::Camera:
    {
        CameraTrack& camera = cinematic.cameras[static_cast<size_t>(row.track)];
        CameraKey key;
        if (!m_throughCinematic || camera.keys.empty())
        {
            // Looking round freely, a new key is what the view sees.
            KeyFromView(context, key, camera.anchor, time);
        }
        else
        {
            // Otherwise what the camera has there already, measured from its anchor.
            const CameraState state = sampler.Camera(camera, time);
            const CinePose frame = sampler.Frame(camera.anchor, time);
            const glm::quat back = glm::inverse(frame.rotation);
            key.position = back * (state.position - frame.position);
            key.rotation = DegreesFromTurn(back * state.rotation);
            key.fov = state.fov;
            key.focus = state.focus;
        }
        key.time = time;
        camera.keys.push_back(key);
        m_selected = {Pick::CameraKey, row.track, static_cast<int>(camera.keys.size()) - 1};
        break;
    }
    case Pick::Actor:
    {
        const std::string actor = cinematic.actors[static_cast<size_t>(row.track)].name;
        const CinePose pose = sampler.Actor(actor, time);
        auto found = std::find_if(cinematic.actorTracks.begin(), cinematic.actorTracks.end(), [&](const ActorTrack& t) { return t.actor == actor; });
        if (found == cinematic.actorTracks.end())
        {
            ActorTrack fresh;
            fresh.actor = actor;
            cinematic.actorTracks.push_back(fresh);
            found = cinematic.actorTracks.end() - 1;
        }
        const CinePose frame = sampler.Frame(found->anchor, time);
        const glm::quat back = glm::inverse(frame.rotation);
        TransformKey key;
        key.time = time;
        key.position = back * (pose.position - frame.position);
        key.rotation = DegreesFromTurn(back * pose.rotation);
        found->keys.push_back(key);
        m_selected = {Pick::ActorKey, static_cast<int>(found - cinematic.actorTracks.begin()), static_cast<int>(found->keys.size()) - 1};
        break;
    }
    case Pick::Sound:
    {
        SoundEvent sound;
        sound.time = time;
        sound.sound = context.sounds.empty() ? std::string() : context.sounds.front();
        cinematic.sounds.push_back(sound);
        m_selected = {Pick::Sound, -1, static_cast<int>(cinematic.sounds.size()) - 1};
        break;
    }
    case Pick::Marker:
        cinematic.markers.push_back({time, "say", ""});
        m_selected = {Pick::Marker, -1, static_cast<int>(cinematic.markers.size()) - 1};
        break;
    case Pick::Text:
    {
        TextItem text;
        text.time = time;
        text.lines = {"{site}", "{planet}"};
        cinematic.texts.push_back(text);
        m_selected = {Pick::Text, -1, static_cast<int>(cinematic.texts.size()) - 1};
        break;
    }
    case Pick::Particle:
    {
        ParticleEvent particle;
        particle.time = time;
        particle.effect = "snow";
        particle.duration = 1.5f;
        cinematic.particles.push_back(particle);
        m_selected = {Pick::Particle, -1, static_cast<int>(cinematic.particles.size()) - 1};
        break;
    }
    case Pick::Light:
    {
        LightTrack& light = cinematic.lights[static_cast<size_t>(row.track)];
        light.intensity.push_back({time, sampler.LightIntensity(light, time), {}});
        m_selected = {Pick::LightKey, row.track, static_cast<int>(light.intensity.size()) - 1};
        break;
    }
    case Pick::Float:
    {
        std::vector<FloatKey>& keys = FloatList(cinematic, row.track);
        keys.push_back({time, SampleFloat(keys, time, kFloatNone[row.track]), {}});
        m_selected = {Pick::Float, row.track, static_cast<int>(keys.size()) - 1};
        break;
    }
    default: return;
    }
    Changed(context);
}

void CinematicEditor::KeyFromView(Context& context, CameraKey& key, const std::string& anchor, float time) const
{
    // Measured from the anchor at that moment, so the key goes wherever the anchor is at another site.
    const CinePose frame = context.player->Active() ? context.player->Sampler().Frame(anchor, time) : CinePose{};
    const glm::quat back = glm::inverse(frame.rotation);
    key.position = back * (m_view.position - frame.position);
    key.rotation = DegreesFromTurn(back * TurnFromDegrees({glm::degrees(m_view.pitch), glm::degrees(m_view.yaw), 0.0f}));
    key.fov = m_view.fovDegrees;
}

void CinematicEditor::ViewFrom(const CameraState& camera)
{
    const glm::vec3 forward = camera.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    m_view.position = camera.position;
    m_view.yaw = std::atan2(forward.x, -forward.z);
    m_view.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    m_view.fovDegrees = camera.fov;
}

std::vector<std::string> CinematicEditor::Anchors(Context& context) const
{
    std::vector<std::string> names{""};
    for (const auto& [name, pose] : context.player->Bindings().anchors)
    {
        names.push_back(name);
    }
    for (const ActorDef& actor : context.player->Playing().actors)
    {
        names.push_back(actor.name);
    }
    return names;
}

std::vector<std::string> CinematicEditor::Unbound(Context& context) const
{
    const Cinematic& cinematic = context.player->Playing();
    const CinematicBindings& bindings = context.player->Bindings();
    std::vector<std::string> missing;
    const auto check = [&](const std::string& name, const char* what)
    {
        if (name.empty() || bindings.anchors.count(name) != 0 || cinematic.FindActor(name) != nullptr)
        {
            return;
        }
        const std::string line = name + " (" + what + ")";
        if (std::find(missing.begin(), missing.end(), line) == missing.end())
        {
            missing.push_back(line);
        }
    };
    for (const CameraTrack& camera : cinematic.cameras)
    {
        check(camera.anchor, "camera");
        check(camera.lookAt, "camera looks at");
    }
    for (const ActorTrack& track : cinematic.actorTracks)
    {
        check(track.anchor, "actor keys");
    }
    for (const SoundEvent& sound : cinematic.sounds)
    {
        check(sound.at, "sound");
    }
    for (const ParticleEvent& particle : cinematic.particles)
    {
        check(particle.at, "particles");
    }
    for (const PathFollow& follow : cinematic.paths)
    {
        if (bindings.paths.count(follow.path) == 0)
        {
            missing.push_back(follow.path + " (path)");
        }
    }
    return missing;
}

// --- The inspector ------------------------------------------------------------------------------------

bool CinematicEditor::DrawChoice(const char* label, std::string& value, const std::vector<std::string>& choices, bool allowOther)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str()))
    {
        if (allowOther)
        {
            ImGui::SetNextItemWidth(-1.0f);
            changed |= InputString("##other", value);
            ImGui::Separator();
        }
        for (const std::string& choice : choices)
        {
            if (ImGui::Selectable(choice.empty() ? "(none)" : choice.c_str(), choice == value))
            {
                value = choice;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool CinematicEditor::DrawEase(Ease& ease, const char* id)
{
    ImGui::PushID(id);
    bool changed = false;
    int kind = static_cast<int>(ease.kind);
    if (ImGui::Combo("Ease", &kind, kEaseNames, 6))
    {
        const auto next = static_cast<Ease::Kind>(kind);
        if (next == Ease::Kind::Curve && ease.kind != Ease::Kind::Curve)
        {
            CurveFor(ease);
        }
        ease.kind = next;
        changed = true;
    }

    // The curve: across is the time between the keys, up is how far between their values. Its handles are dragged;
    // dragging one makes the ease a hand-shaped curve, starting from the named one it was.
    const float size = std::min(ImGui::GetContentRegionAvail().x, 200.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const auto P = [&](const glm::vec2& v) { return ImVec2(origin.x + v.x * size, origin.y + (1.25f - v.y) / 1.5f * size); };
    const auto V = [&](const ImVec2& p) { return glm::vec2((p.x - origin.x) / size, 1.25f - (p.y - origin.y) / size * 1.5f); };
    ImGui::InvisibleButton("##curve", {size, size});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size, origin.y + size}, IM_COL32(24, 26, 30, 255), 4.0f);
    draw->AddRectFilled(P({0.0f, 1.0f}), P({1.0f, 0.0f}), IM_COL32(34, 38, 44, 255));
    draw->AddLine(P({0.0f, 0.0f}), P({1.0f, 1.0f}), IM_COL32(60, 64, 72, 255));
    static int s_handle = 0;
    if (ImGui::IsItemActivated())
    {
        if (ease.kind != Ease::Kind::Curve)
        {
            CurveFor(ease);
            ease.kind = Ease::Kind::Curve;
            changed = true;
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const auto distance = [&](const glm::vec2& v)
        {
            const ImVec2 p = P(v);
            return std::hypot(p.x - mouse.x, p.y - mouse.y);
        };
        s_handle = distance(ease.a) <= distance(ease.b) ? 1 : 2;
    }
    if (ImGui::IsItemActive() && s_handle != 0)
    {
        glm::vec2 v = V(ImGui::GetIO().MousePos);
        v.x = std::clamp(v.x, 0.0f, 1.0f);
        v.y = std::clamp(v.y, -0.5f, 1.5f);
        (s_handle == 1 ? ease.a : ease.b) = v;
        changed = true;
    }
    else if (!ImGui::IsItemActive())
    {
        s_handle = 0;
    }
    ImVec2 last = P({0.0f, 0.0f});
    for (int i = 1; i <= 64; ++i)
    {
        const float t = static_cast<float>(i) / 64.0f;
        const ImVec2 p = P({t, ease.Apply(t)});
        draw->AddLine(last, p, IM_COL32(236, 196, 84, 255), 2.0f);
        last = p;
    }
    if (ease.kind == Ease::Kind::Curve)
    {
        draw->AddLine(P({0.0f, 0.0f}), P(ease.a), IM_COL32(150, 176, 214, 200));
        draw->AddLine(P({1.0f, 1.0f}), P(ease.b), IM_COL32(150, 176, 214, 200));
        draw->AddCircleFilled(P(ease.a), 5.0f, IM_COL32(150, 176, 214, 255));
        draw->AddCircleFilled(P(ease.b), 5.0f, IM_COL32(150, 176, 214, 255));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    const auto preset = [&](const char* name, glm::vec2 a, glm::vec2 b)
    {
        if (ImGui::SmallButton(name))
        {
            ease.kind = Ease::Kind::Curve;
            ease.a = a;
            ease.b = b;
            changed = true;
        }
    };
    ImGui::TextDisabled("Shapes");
    preset("Gentle", {0.25f, 0.1f}, {0.25f, 1.0f});
    preset("Hard in", {0.7f, 0.0f}, {0.84f, 0.0f});
    preset("Hard out", {0.16f, 1.0f}, {0.3f, 1.0f});
    preset("Settle", {0.34f, 1.4f}, {0.64f, 1.0f});
    preset("Wind up", {0.36f, -0.4f}, {0.66f, -0.1f});
    ImGui::EndGroup();
    if (ease.kind == Ease::Kind::Curve)
    {
        float handles[4] = {ease.a.x, ease.a.y, ease.b.x, ease.b.y};
        if (ImGui::DragFloat4("Handles", handles, 0.005f, -0.5f, 1.5f, "%.2f"))
        {
            ease.a = {std::clamp(handles[0], 0.0f, 1.0f), handles[1]};
            ease.b = {std::clamp(handles[2], 0.0f, 1.0f), handles[3]};
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

void CinematicEditor::DrawSettings(Context& context)
{
    Cinematic& cinematic = Edited(context);
    bool changed = false;
    ImGui::SeparatorText("Cinematic");
    std::string name = cinematic.name;
    if (InputString("Name", name) && !name.empty())
    {
        cinematic.name = name;
        m_editing = name;
        changed = true;
    }
    changed |= ImGui::DragFloat("Length", &cinematic.duration, 0.05f, 0.5f, 600.0f, "%.2f s");
    changed |= ImGui::Checkbox("Stops the world while it plays", &cinematic.pausesGameplay);
    changed |= ImGui::DragFloat("Hand back over", &cinematic.blendOut, 0.01f, 0.0f, 10.0f, "%.2f s");
    changed |= ImGui::Checkbox("Loops", &cinematic.loop);

    ImGui::SeparatorText("Cameras");
    for (size_t i = 0; i < cinematic.cameras.size(); ++i)
    {
        const CameraTrack& camera = cinematic.cameras[i];
        if (ImGui::Selectable((camera.name + "##cam" + std::to_string(i)).c_str()))
        {
            m_selected = {Pick::Camera, static_cast<int>(i), -1};
        }
        ImGui::SameLine(170.0f);
        ImGui::TextDisabled("%zu keys%s%s", camera.keys.size(), camera.anchor.empty() ? "" : (", from " + camera.anchor).c_str(),
                            camera.lookAt.empty() ? "" : (", at " + camera.lookAt).c_str());
    }
    ImGui::SeparatorText("Actors");
    for (size_t i = 0; i < cinematic.actors.size(); ++i)
    {
        const ActorDef& actor = cinematic.actors[i];
        if (ImGui::Selectable((actor.name + "##actor" + std::to_string(i)).c_str()))
        {
            m_selected = {Pick::Actor, static_cast<int>(i), -1};
        }
        ImGui::SameLine(170.0f);
        ImGui::TextDisabled("%s", actor.bind.empty() ? actor.model.c_str() : ("the site's " + actor.bind).c_str());
    }
    ImGui::TextDisabled("+ Add on the timeline adds cameras, actors and the rest.");

    ImGui::SeparatorText("What this place supplies");
    const CinematicBindings& bindings = context.player->Bindings();
    for (const auto& [anchor, pose] : bindings.anchors)
    {
        ImGui::BulletText("%s", anchor.c_str());
        ImGui::SameLine(190.0f);
        ImGui::TextDisabled("%.1f %.1f %.1f", pose.position.x, pose.position.y, pose.position.z);
    }
    for (const auto& [path, points] : bindings.paths)
    {
        ImGui::BulletText("path %s", path.c_str());
        ImGui::SameLine(190.0f);
        ImGui::TextDisabled("%zu points", points.size());
    }
    for (const std::string& missing : Unbound(context))
    {
        ImGui::TextColored({1.0f, 0.55f, 0.35f, 1.0f}, "Not here: %s", missing.c_str());
    }

    ImGui::SeparatorText("Keys");
    ImGui::TextDisabled("Space play/pause   Left/Right a frame (Shift a second)\n"
                        "Home/End   , and . the key before/after\n"
                        "K key at the playhead   Del delete\n"
                        "Ctrl+D duplicate at playhead   Alt+drag copy\n"
                        "Ctrl+C/V copy/paste values   Ctrl+Z/Y undo\n"
                        "Ctrl+S save   C free camera   F fit   H hide\n"
                        "Timeline: double-click adds, right-click menu,\n"
                        "Shift unsnaps, Ctrl+wheel zooms, middle-drag pans\n"
                        "Free camera: right-drag looks, WASD, Q/E, Shift");
    if (changed)
    {
        Changed(context);
    }
}

void CinematicEditor::DrawInspector(Context& context)
{
    Cinematic& cinematic = Edited(context);
    const float playhead = context.player->Time();
    const CinematicSampler sampler = context.player->Sampler();
    if (m_selected.kind == Pick::None)
    {
        DrawSettings(context);
        return;
    }
    if (ImGui::SmallButton("< The whole cinematic"))
    {
        m_selected = {};
        return;
    }
    const std::vector<std::string> anchors = Anchors(context);
    std::vector<std::string> actorNames;
    for (const ActorDef& actor : cinematic.actors)
    {
        actorNames.push_back(actor.name);
    }
    std::vector<std::string> cameraNames;
    for (const CameraTrack& camera : cinematic.cameras)
    {
        cameraNames.push_back(camera.name);
    }
    const auto item = static_cast<size_t>(std::max(m_selected.item, 0));
    const auto track = static_cast<size_t>(std::max(m_selected.track, 0));
    const bool one = m_selected.item >= 0;
    bool changed = false;
    const auto timeField = [&](float& time) { changed |= ImGui::DragFloat("Time", &time, 0.01f, 0.0f, 3600.0f, "%.2f s"); };
    const auto rowButton = [&](const char* what, Pick kind, int rowTrack)
    {
        if (ImGui::Button(what))
        {
            AddAt(context, {"", kind, rowTrack}, playhead);
        }
    };
    // Where to look at something from: a little back from it and up.
    const auto lookAtThing = [&](const glm::vec3& at, float distance)
    {
        const glm::vec3 forward = glm::normalize(glm::vec3(std::sin(m_view.yaw), 0.0f, -std::cos(m_view.yaw)));
        CameraState camera;
        camera.position = at - forward * distance + glm::vec3(0.0f, distance * 0.4f, 0.0f);
        const glm::vec3 look = glm::normalize(at - camera.position);
        camera.rotation = TurnFromDegrees({glm::degrees(std::asin(look.y)), glm::degrees(std::atan2(look.x, -look.z)), 0.0f});
        camera.fov = m_view.fovDegrees;
        ViewFrom(camera);
        m_throughCinematic = false;
    };

    switch (m_selected.kind)
    {
    case Pick::Camera:
    {
        CameraTrack& camera = cinematic.cameras[track];
        ImGui::SeparatorText("Camera");
        std::string name = camera.name;
        if (InputString("Name", name) && !name.empty() && cinematic.FindCamera(name) == nullptr)
        {
            for (Shot& shot : cinematic.shots)
            {
                if (shot.camera == camera.name)
                {
                    shot.camera = name;
                }
            }
            camera.name = name;
            changed = true;
        }
        changed |= DrawChoice("Measured from", camera.anchor, anchors, true);
        changed |= DrawChoice("Looks at", camera.lookAt, anchors, true);
        if (!camera.lookAt.empty())
        {
            changed |= ImGui::DragFloat3("Look offset", &camera.lookOffset.x, 0.05f);
        }
        ImGui::TextDisabled("%zu keys", camera.keys.size());
        rowButton("Key at playhead (K)", Pick::Camera, m_selected.track);
        ImGui::SameLine();
        if (ImGui::Button("Cut to it at playhead"))
        {
            cinematic.shots.push_back({playhead, camera.name, 0.0f, {}});
            m_selected = {Pick::Shot, -1, static_cast<int>(cinematic.shots.size()) - 1};
            changed = true;
        }
        if (ImGui::Button("Look through it now"))
        {
            ViewFrom(sampler.Camera(camera, playhead));
            m_throughCinematic = false;
        }
        break;
    }
    case Pick::CameraKey:
    {
        CameraTrack& camera = cinematic.cameras[track];
        CameraKey& key = camera.keys[item];
        ImGui::SeparatorText(("Key of " + camera.name).c_str());
        timeField(key.time);
        changed |= ImGui::DragFloat3("Position", &key.position.x, 0.02f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat3(camera.lookAt.empty() ? "Turn" : "Turn (roll only)", &key.rotation.x, 0.2f, -720.0f, 720.0f, "%.1f");
        changed |= ImGui::SliderFloat("Field of view", &key.fov, 10.0f, 120.0f, "%.0f");
        changed |= ImGui::DragFloat("Focus", &key.focus, 0.1f, 0.0f, 1000.0f, "%.1f m");
        ImGui::TextDisabled("%s", camera.anchor.empty() ? "In the world." : ("Measured from " + camera.anchor + ".").c_str());
        if (ImGui::Button("Set from the view"))
        {
            KeyFromView(context, key, camera.anchor, key.time);
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Where the free camera is and where it looks, measured from %s.", camera.anchor.empty() ? "the world" : camera.anchor.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("View from it"))
        {
            ViewFrom(sampler.Camera(camera, key.time));
            m_throughCinematic = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Go to it"))
        {
            Seek(context, key.time);
        }
        ImGui::SeparatorText("Easing into this key");
        changed |= DrawEase(key.ease, "ease");
        break;
    }
    case Pick::Shot:
    {
        if (!one)
        {
            ImGui::SeparatorText("Shots");
            ImGui::TextDisabled("Which camera the picture is from, from when.");
            rowButton("Cut at playhead", Pick::Shot, -1);
            break;
        }
        Shot& shot = cinematic.shots[item];
        ImGui::SeparatorText("Shot");
        timeField(shot.time);
        changed |= DrawChoice("Camera", shot.camera, cameraNames, false);
        changed |= ImGui::DragFloat("Blend in over", &shot.blend, 0.01f, 0.0f, 30.0f, "%.2f s");
        ImGui::TextDisabled("%s", shot.blend > 0.0f ? "Moves from the camera before to this one." : "A cut.");
        if (shot.blend > 0.0f)
        {
            changed |= DrawEase(shot.ease, "shot");
        }
        break;
    }
    case Pick::Actor:
    {
        ActorDef& actor = cinematic.actors[track];
        ImGui::SeparatorText("Actor");
        std::string name = actor.name;
        if (InputString("Name", name) && !name.empty() && cinematic.FindActor(name) == nullptr)
        {
            // Everything that names it follows.
            const std::string was = actor.name;
            const auto rename = [&](std::string& value)
            {
                if (value == was)
                {
                    value = name;
                }
            };
            for (ActorTrack& t : cinematic.actorTracks)
            {
                rename(t.actor), rename(t.anchor);
            }
            for (PathFollow& p : cinematic.paths)
            {
                rename(p.actor);
            }
            for (ClipEvent& c : cinematic.clips)
            {
                rename(c.actor);
            }
            for (CameraTrack& c : cinematic.cameras)
            {
                rename(c.anchor), rename(c.lookAt);
            }
            for (SoundEvent& s : cinematic.sounds)
            {
                rename(s.at);
            }
            for (ParticleEvent& p : cinematic.particles)
            {
                rename(p.at);
            }
            actor.name = name;
            changed = true;
            m_needsReplay = true;
        }
        std::vector<std::string> binds{"", "site_shuttle", "site_crawler"};
        if (DrawChoice("The site's own", actor.bind, binds, true))
        {
            changed = true;
            m_needsReplay = true;
        }
        if (actor.bind.empty() && DrawChoice("Model", actor.model, ListModels(), true))
        {
            changed = true;
            m_needsReplay = true;
        }
        const CinePose pose = sampler.Actor(actor.name, playhead);
        const glm::vec3 degrees = DegreesFromTurn(pose.rotation);
        ImGui::TextDisabled("Now at %.1f %.1f %.1f, turned %.0f", pose.position.x, pose.position.y, pose.position.z, degrees.y);
        for (ActorTrack& t : cinematic.actorTracks)
        {
            if (t.actor == actor.name)
            {
                changed |= DrawChoice("Keys measured from", t.anchor, anchors, true);
            }
        }
        rowButton("Key at playhead (K)", Pick::Actor, m_selected.track);
        ImGui::SameLine();
        if (ImGui::Button("Look at it"))
        {
            lookAtThing(pose.position, 14.0f);
        }
        if (ImGui::Button("Drive a path from playhead"))
        {
            PathFollow follow;
            follow.actor = actor.name;
            const auto& paths = context.player->Bindings().paths;
            follow.path = paths.empty() ? std::string("route") : paths.begin()->first;
            follow.start = playhead;
            follow.end = playhead + 10.0f;
            cinematic.paths.push_back(follow);
            m_selected = {Pick::Path, -1, static_cast<int>(cinematic.paths.size()) - 1};
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clip at playhead"))
        {
            ClipEvent clip;
            clip.time = playhead;
            clip.actor = actor.name;
            const std::vector<std::string> clips = context.clipsOf(actor.name);
            clip.clip = clips.empty() ? std::string() : clips.front();
            cinematic.clips.push_back(clip);
            m_selected = {Pick::Clip, -1, static_cast<int>(cinematic.clips.size()) - 1};
            changed = true;
        }
        break;
    }
    case Pick::ActorKey:
    {
        ActorTrack& keys = cinematic.actorTracks[track];
        TransformKey& key = keys.keys[item];
        ImGui::SeparatorText(("Key of " + keys.actor).c_str());
        timeField(key.time);
        changed |= ImGui::DragFloat3("Position", &key.position.x, 0.02f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat3("Turn", &key.rotation.x, 0.2f, -720.0f, 720.0f, "%.1f");
        changed |= DrawChoice("Measured from", keys.anchor, anchors, true);
        if (ImGui::Button("Put it where the view looks"))
        {
            glm::vec3 at;
            if (context.pick && context.pick(at))
            {
                const CinePose frame = sampler.Frame(keys.anchor, key.time);
                key.position = glm::inverse(frame.rotation) * (at - frame.position);
                changed = true;
            }
            else
            {
                m_status = "The view is not looking at anything";
                m_statusAge = 0.0f;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Go to it"))
        {
            Seek(context, key.time);
        }
        ImGui::SameLine();
        if (ImGui::Button("Look at it"))
        {
            lookAtThing(sampler.Actor(keys.actor, key.time).position, 14.0f);
        }
        ImGui::SeparatorText("Easing into this key");
        changed |= DrawEase(key.ease, "ease");
        break;
    }
    case Pick::Path:
    {
        PathFollow& follow = cinematic.paths[item];
        ImGui::SeparatorText("Driving a path");
        changed |= DrawChoice("Actor", follow.actor, actorNames, false);
        std::vector<std::string> paths;
        for (const auto& [name, points] : context.player->Bindings().paths)
        {
            paths.push_back(name);
        }
        changed |= DrawChoice("Path", follow.path, paths, true);
        changed |= ImGui::DragFloat("Sets off", &follow.start, 0.01f, 0.0f, 3600.0f, "%.2f s");
        changed |= ImGui::DragFloat("Arrives", &follow.end, 0.01f, 0.0f, 3600.0f, "%.2f s");
        follow.end = std::max(follow.end, follow.start + 0.05f);
        changed |= ImGui::DragFloat("Over the ground", &follow.lift, 0.01f, -5.0f, 50.0f, "%.2f m");
        changed |= ImGui::Checkbox("Faces the way it goes", &follow.face);
        changed |= ImGui::Checkbox("Backing along it", &follow.reverse);
        ImGui::TextDisabled("The path is the game's: it is wherever this site puts it.");
        changed |= DrawEase(follow.ease, "path");
        break;
    }
    case Pick::Clip:
    {
        ClipEvent& clip = cinematic.clips[item];
        ImGui::SeparatorText("Clip");
        timeField(clip.time);
        changed |= DrawChoice("Actor", clip.actor, actorNames, false);
        changed |= DrawChoice("Clip", clip.clip, context.clipsOf(clip.actor), true);
        changed |= ImGui::DragFloat("Speed", &clip.speed, 0.01f, 0.05f, 10.0f, "%.2fx");
        break;
    }
    case Pick::Sound:
    {
        if (!one)
        {
            ImGui::SeparatorText("Sounds");
            rowButton("Sound at playhead", Pick::Sound, -1);
            break;
        }
        SoundEvent& sound = cinematic.sounds[item];
        ImGui::SeparatorText("Sound");
        timeField(sound.time);
        changed |= DrawChoice("Sound", sound.sound, context.sounds, true);
        ImGui::SameLine();
        if (ImGui::SmallButton("Hear") && context.hear)
        {
            context.hear(sound.sound);
        }
        changed |= ImGui::SliderFloat("Volume", &sound.volume, 0.0f, 2.0f, "%.2f");
        changed |= DrawChoice("From", sound.at, anchors, true);
        changed |= ImGui::Checkbox("Music (heard however far)", &sound.music);
        ImGui::TextDisabled("%s", sound.at.empty() ? "In the head." : "From there, as far as it carries.");
        break;
    }
    case Pick::Marker:
    {
        if (!one)
        {
            ImGui::SeparatorText("Markers");
            rowButton("Marker at playhead", Pick::Marker, -1);
            break;
        }
        pred::Marker& marker = cinematic.markers[item];
        ImGui::SeparatorText("Marker");
        timeField(marker.time);
        changed |= DrawChoice("Does", marker.name, {"say", "hold", "release", "go_to_ship"}, true);
        changed |= InputString("Value", marker.value);
        const char* help = marker.name == "say"          ? "The intercom says a line: its name in intercom.json."
                           : marker.name == "hold"       ? "The players are held from here, aboard."
                           : marker.name == "release"    ? "The players have their bodies back."
                           : marker.name == "go_to_ship" ? "Everybody back to the ship (not while editing)."
                                                         : "The game decides what this means.";
        ImGui::TextDisabled("%s", help);
        break;
    }
    case Pick::Text:
    {
        if (!one)
        {
            ImGui::SeparatorText("Words");
            rowButton("Title card at playhead", Pick::Text, -1);
            break;
        }
        TextItem& text = cinematic.texts[item];
        ImGui::SeparatorText("Words");
        timeField(text.time);
        changed |= ImGui::DragFloat("Shown for", &text.duration, 0.01f, 0.1f, 120.0f, "%.2f s");
        changed |= DrawChoice("Style", text.style, {"title_card", "caption"}, false);
        changed |= ImGui::DragFloat("Letters a second", &text.typeSpeed, 0.2f, 0.0f, 200.0f, "%.0f");
        char buffer[1024] = {};
        const std::string joined = Joined(text.lines);
        joined.copy(buffer, std::min(joined.size(), sizeof(buffer) - 1));
        if (ImGui::InputTextMultiline("Lines", buffer, sizeof(buffer), {-1.0f, 90.0f}))
        {
            text.lines = Split(buffer);
            changed = true;
        }
        ImGui::TextDisabled("Filled in: {site} {planet} {region} {local_time} {conditions}");
        for (size_t line = 0; line < text.lines.size(); ++line)
        {
            ImGui::TextDisabled("> %s", sampler.Fill(text.lines[line]).c_str());
        }
        break;
    }
    case Pick::Particle:
    {
        if (!one)
        {
            ImGui::SeparatorText("Particles");
            rowButton("Particles at playhead", Pick::Particle, -1);
            break;
        }
        ParticleEvent& particle = cinematic.particles[item];
        ImGui::SeparatorText("Particles");
        timeField(particle.time);
        changed |= DrawChoice("Effect", particle.effect, {"snow", "dust", "exhaust"}, true);
        changed |= DrawChoice("At", particle.at, anchors, true);
        changed |= ImGui::DragFloat3("Offset", &particle.offset.x, 0.05f, 0.0f, 0.0f, "%.2f");
        changed |= ImGui::DragFloat("Lasting", &particle.duration, 0.01f, 0.1f, 30.0f, "%.2f s");
        break;
    }
    case Pick::Light:
    {
        LightTrack& light = cinematic.lights[track];
        ImGui::SeparatorText("Light");
        changed |= InputString("Light", light.light);
        ImGui::TextDisabled("%zu keys", light.intensity.size());
        rowButton("Key at playhead (K)", Pick::Light, m_selected.track);
        break;
    }
    case Pick::LightKey:
    case Pick::Float:
    {
        if (!one)
        {
            ImGui::SeparatorText(kFloatNames[m_selected.track]);
            ImGui::TextDisabled("Now %.2f", SampleFloat(FloatList(cinematic, m_selected.track), playhead, kFloatNone[m_selected.track]));
            rowButton("Key at playhead (K)", Pick::Float, m_selected.track);
            break;
        }
        FloatKey& key = m_selected.kind == Pick::Float ? FloatList(cinematic, m_selected.track)[item] : cinematic.lights[track].intensity[item];
        ImGui::SeparatorText(m_selected.kind == Pick::Float ? kFloatNames[m_selected.track] : cinematic.lights[track].light.c_str());
        timeField(key.time);
        changed |= ImGui::DragFloat("Value", &key.value, 0.01f, -100.0f, 100.0f, "%.3f");
        if (m_selected.kind == Pick::Float && (m_selected.track == 2 || m_selected.track == 3))
        {
            ImGui::TextDisabled("0 none, 1 all.");
        }
        else if (m_selected.kind == Pick::Float && m_selected.track >= 4)
        {
            ImGui::TextDisabled("1 as it is, 2 twice.");
        }
        ImGui::SeparatorText("Easing into this key");
        changed |= DrawEase(key.ease, "ease");
        break;
    }
    case Pick::None: break;
    }
    if (changed)
    {
        Changed(context);
    }
}

// --- Keys ----------------------------------------------------------------------------------------------

void CinematicEditor::Shortcuts(Context& context)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
    {
        return;
    }
    Cinematic& cinematic = Edited(context);
    CinematicPlayer& player = *context.player;
    const float playhead = player.Time();
    const bool ctrl = io.KeyCtrl;
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
    const auto repeated = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };

    if (pressed(ImGuiKey_Space))
    {
        PlayPause(context);
    }
    if (repeated(ImGuiKey_LeftArrow) || repeated(ImGuiKey_RightArrow))
    {
        player.SetPaused(true);
        const float step = io.KeyShift ? 1.0f : kFrame;
        Seek(context, playhead + (ImGui::IsKeyDown(ImGuiKey_RightArrow) ? step : -step));
    }
    if (pressed(ImGuiKey_Home))
    {
        Seek(context, 0.0f);
    }
    if (pressed(ImGuiKey_End))
    {
        Seek(context, cinematic.duration);
    }
    if (pressed(ImGuiKey_Comma) || pressed(ImGuiKey_Period))
    {
        // The key before or after, on any row.
        const bool forward = ImGui::IsKeyDown(ImGuiKey_Period);
        float best = forward ? cinematic.duration : 0.0f;
        for (const Row& row : Rows(cinematic))
        {
            for (const Item& item : ItemsOf(row, cinematic))
            {
                if (forward ? item.time > playhead + 1.0e-3f && item.time < best : item.time < playhead - 1.0e-3f && item.time > best)
                {
                    best = item.time;
                }
            }
        }
        player.SetPaused(true);
        Seek(context, best);
    }
    if (pressed(ImGuiKey_Delete))
    {
        Delete(context);
    }
    if (ctrl && pressed(ImGuiKey_D))
    {
        float at = playhead;
        if (std::abs(at - TimeOf(cinematic, m_selected)) < 1.0e-4f)
        {
            at += std::max(m_snapStep, kFrame);
        }
        Duplicate(context, at);
    }
    if (ctrl && pressed(ImGuiKey_C))
    {
        Copy(cinematic);
    }
    if (ctrl && pressed(ImGuiKey_V))
    {
        Paste(context, playhead);
    }
    if (ctrl && pressed(ImGuiKey_Z))
    {
        io.KeyShift ? Redo(context) : Undo(context);
    }
    if (ctrl && pressed(ImGuiKey_Y))
    {
        Redo(context);
    }
    if (ctrl && pressed(ImGuiKey_S))
    {
        if (m_pending)
        {
            Commit(context);
        }
        Cinematic copy = cinematic;
        copy.Tidy();
        if (Fileable(copy.name) && copy.SaveToFile(context.folder / (copy.name + ".json")))
        {
            (*context.library)[copy.name] = copy;
            m_saved = copy.ToJsonText();
            m_committed = m_saved;
            m_status = "Saved " + copy.name + ".json";
        }
        else
        {
            m_status = "Could not save " + copy.name;
        }
        m_statusAge = 0.0f;
    }
    if (!ctrl && pressed(ImGuiKey_K))
    {
        // A key on the row of what is selected.
        for (const Row& row : Rows(cinematic))
        {
            if (OnRow<Row, Selection, Pick>(cinematic, row, m_selected) && row.kind != Pick::Shot && row.kind != Pick::Sound && row.kind != Pick::Marker &&
                row.kind != Pick::Text && row.kind != Pick::Particle)
            {
                AddAt(context, row, playhead);
                break;
            }
        }
    }
    if (!ctrl && pressed(ImGuiKey_C))
    {
        m_throughCinematic = !m_throughCinematic;
        if (!m_throughCinematic)
        {
            ViewFrom(player.Picture());
        }
    }
    if (!ctrl && pressed(ImGuiKey_F))
    {
        m_fitted = false;
    }
    if (!ctrl && pressed(ImGuiKey_H))
    {
        m_hidden = !m_hidden;
    }
}

} // namespace pred
