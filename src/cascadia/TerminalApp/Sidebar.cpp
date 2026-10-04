// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Sidebar.cpp
//
// A vertical replacement for the tab strip. Every pane of every tab is listed,
// grouped by the git repository it's in, and within that by worktree. Panes
// running Claude Code additionally show the state of that session (working,
// waiting for approval, done, background tasks), which Claude Code reports
// through hooks into %USERPROFILE%\.claude\terminal-status\<WT_SESSION>.json.
// See tools/claude-sidebar for the hook side of that contract.
//
// The TabView is still the source of truth for tabs and selection; its items
// are merely collapsed while the sidebar is enabled.

#include "pch.h"
#include "TerminalPage.h"

#include <deque>
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace ::Microsoft::Console;

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}

namespace
{
    constexpr double SidebarDefaultWidth = 260.0;
    constexpr double SidebarMinWidth = 170.0;
    constexpr double SidebarMaxWidth = 560.0;
    constexpr double SidebarCompactWidth = 48.0;
    constexpr auto SidebarPollInterval = std::chrono::milliseconds{ 750 };
    constexpr ULONGLONG GitCacheLifetimeMs = 3000;

    constexpr std::wstring_view IconFont{ L"Segoe Fluent Icons,Segoe MDL2 Assets" };
    constexpr std::wstring_view GlyphChevronDown{ L"\xE70D" };
    constexpr std::wstring_view GlyphChevronRight{ L"\xE76C" };
    constexpr std::wstring_view GlyphAdd{ L"\xE710" };
    constexpr std::wstring_view GlyphTerminal{ L"\xE756" };
    constexpr std::wstring_view GlyphHome{ L"\xE80F" };
    constexpr std::wstring_view GlyphFolder{ L"\xE8B7" };
    constexpr std::wstring_view GlyphStopwatch{ L"\xE916" };

    struct ClaudeStatus
    {
        bool present{ false };
        std::wstring state; // "working" | "waiting" | "idle" | "error"
        std::wstring detail;
        std::wstring cwd;
        int64_t ts{ 0 };
        uint32_t pid{ 0 };
        int subagents{ 0 };
        std::vector<std::wstring> backgroundTasks;
    };

    struct GitInfo
    {
        bool isGit{ false };
        std::wstring repoRoot;
        std::wstring worktreeRoot;
        std::wstring branch;
    };

    struct PaneEntry
    {
        winrt::TerminalApp::Tab tab{ nullptr };
        uint32_t paneId{ 0 };
        bool active{ false };
        std::wstring title;
        std::wstring cwd;
        std::wstring sessionId;
        GitInfo git;
        ClaudeStatus claude;
        bool unseen{ false };
    };

    struct WorktreeGroup
    {
        std::wstring key;
        std::wstring root;
        std::wstring branch;
        bool isMain{ false };
        std::vector<size_t> entries;
    };

    struct RepoGroup
    {
        std::wstring key;
        std::wstring name;
        std::wstring root;
        bool isGit{ false };
        std::vector<WorktreeGroup> worktrees;
    };

    // Ordered by how much the group needs the user's attention.
    enum class Attention
    {
        None = 0,
        Unseen,
        Working,
        Error,
        Waiting,
    };

    struct Palette
    {
        Color background{};
        bool isLight{ false };
        Brush foreground{ nullptr };
        Brush secondary{ nullptr };
        Brush hover{ nullptr };
        Brush pressed{ nullptr };
        Brush selected{ nullptr };
        Brush separator{ nullptr };
        Brush accent{ nullptr };
        Brush accentSubtle{ nullptr };
        Brush waiting{ nullptr };
        Brush done{ nullptr };
        Brush error{ nullptr };
        Brush transparent{ nullptr };
    };

    std::wstring _toLower(std::wstring s)
    {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    }

    std::wstring _trim(std::wstring_view s)
    {
        const auto begin = s.find_first_not_of(L" \t\r\n");
        if (begin == std::wstring_view::npos)
        {
            return {};
        }
        const auto end = s.find_last_not_of(L" \t\r\n");
        return std::wstring{ s.substr(begin, end - begin + 1) };
    }

    // Accepts Windows paths, MSYS paths (/c/foo) and file:// URIs, and returns
    // a Windows path without a trailing separator.
    std::wstring _normalizePath(std::wstring_view input)
    {
        auto p = _trim(input);
        if (p.starts_with(L"file://"))
        {
            p = p.substr(7);
            // file://host/C:/foo or file:///C:/foo
            if (const auto slash = p.find(L'/'); slash != std::wstring::npos)
            {
                p = p.substr(slash);
            }
            if (p.size() >= 3 && p[0] == L'/' && p[2] == L':')
            {
                p = p.substr(1);
            }
        }
        if (p.size() >= 2 && p[0] == L'/' && iswalpha(p[1]) && (p.size() == 2 || p[2] == L'/'))
        {
            p = std::wstring{ static_cast<wchar_t>(towupper(p[1])) } + L":" + (p.size() > 2 ? p.substr(2) : L"\\");
        }
        std::replace(p.begin(), p.end(), L'/', L'\\');
        if (p.size() >= 2 && p[1] == L':')
        {
            p[0] = static_cast<wchar_t>(towupper(p[0]));
        }
        while (p.size() > 3 && p.back() == L'\\')
        {
            p.pop_back();
        }
        return p;
    }

    std::wstring _leafName(const std::wstring& path)
    {
        const auto trimmed = path.ends_with(L'\\') ? path.substr(0, path.size() - 1) : path;
        const auto slash = trimmed.find_last_of(L'\\');
        return slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1);
    }

    std::optional<std::string> _readSmallFile(const std::filesystem::path& path)
    {
        wil::unique_hfile file{ CreateFileW(path.c_str(),
                                            GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr) };
        if (!file)
        {
            return std::nullopt;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart > 1024 * 1024)
        {
            return std::nullopt;
        }
        std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!buffer.empty() && !ReadFile(file.get(), buffer.data(), gsl::narrow_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            return std::nullopt;
        }
        buffer.resize(read);
        return buffer;
    }

    std::wstring _readBranch(const std::filesystem::path& gitDir)
    {
        const auto head = _readSmallFile(gitDir / L"HEAD");
        if (!head)
        {
            return {};
        }
        const auto text = _trim(til::u8u16(*head));
        static constexpr std::wstring_view refPrefix{ L"ref: refs/heads/" };
        if (text.starts_with(refPrefix))
        {
            return text.substr(refPrefix.size());
        }
        if (text.size() >= 7)
        {
            return text.substr(0, 7) + L" (detached)";
        }
        return {};
    }

    // Walks up from `dir` looking for a .git directory (a repository's main
    // worktree) or a .git file (a linked worktree or a submodule).
    GitInfo _resolveGit(const std::wstring& dir)
    {
        GitInfo info;
        if (dir.size() < 2 || dir[1] != L':')
        {
            // Not a local drive path (WSL, UNC, unknown). Don't touch the disk.
            return info;
        }

        std::filesystem::path current{ dir };
        for (auto depth = 0; depth < 64; ++depth)
        {
            const auto dotGit = current / L".git";
            const auto attributes = GetFileAttributesW(dotGit.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                std::filesystem::path gitDir;
                info.isGit = true;
                info.worktreeRoot = _normalizePath(current.wstring());
                info.repoRoot = info.worktreeRoot;

                if (WI_IsFlagSet(attributes, FILE_ATTRIBUTE_DIRECTORY))
                {
                    gitDir = dotGit;
                }
                else if (const auto content = _readSmallFile(dotGit))
                {
                    const auto text = _trim(til::u8u16(*content));
                    static constexpr std::wstring_view gitdirPrefix{ L"gitdir:" };
                    if (text.starts_with(gitdirPrefix))
                    {
                        std::filesystem::path target{ _normalizePath(text.substr(gitdirPrefix.size())) };
                        if (target.is_relative())
                        {
                            target = current / target;
                        }
                        gitDir = target.lexically_normal();

                        // Linked worktrees live in <common dir>/worktrees/<name>.
                        const auto parent = gitDir.parent_path();
                        if (_wcsicmp(parent.filename().c_str(), L"worktrees") == 0)
                        {
                            auto repo = parent.parent_path();
                            // <repo>/.git or a bare <repo>/.bare layout
                            const auto name = repo.filename().wstring();
                            if (!name.empty() && name[0] == L'.')
                            {
                                repo = repo.parent_path();
                            }
                            info.repoRoot = _normalizePath(repo.wstring());
                        }
                    }
                }

                if (!gitDir.empty())
                {
                    info.branch = _readBranch(gitDir);
                }
                return info;
            }

            const auto parent = current.parent_path();
            if (parent.empty() || parent == current)
            {
                break;
            }
            current = parent;
        }
        return info;
    }

    bool _processAlive(const uint32_t pid)
    {
        if (pid == 0)
        {
            return true;
        }
        wil::unique_handle process{ OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
        if (!process)
        {
            // Can't tell. Only "no such process" means it's gone.
            return GetLastError() != ERROR_INVALID_PARAMETER;
        }
        return WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;
    }

    ClaudeStatus _readClaudeStatus(const std::filesystem::path& file)
    {
        ClaudeStatus status;
        const auto content = _readSmallFile(file);
        if (!content || content->empty())
        {
            return status;
        }

        try
        {
            using namespace winrt::Windows::Data::Json;
            JsonObject obj{ nullptr };
            if (!JsonObject::TryParse(winrt::to_hstring(*content), obj))
            {
                return status;
            }

            const auto getString = [&](const wchar_t* name) -> std::wstring {
                if (obj.HasKey(name) && obj.GetNamedValue(name).ValueType() == JsonValueType::String)
                {
                    return std::wstring{ obj.GetNamedString(name) };
                }
                return {};
            };
            const auto getNumber = [&](const wchar_t* name) -> double {
                if (obj.HasKey(name) && obj.GetNamedValue(name).ValueType() == JsonValueType::Number)
                {
                    return obj.GetNamedNumber(name);
                }
                return 0;
            };

            status.state = getString(L"state");
            status.detail = getString(L"detail");
            status.cwd = getString(L"cwd");
            status.ts = static_cast<int64_t>(getNumber(L"ts"));
            status.pid = static_cast<uint32_t>(getNumber(L"pid"));
            status.subagents = static_cast<int>(getNumber(L"subagents"));

            if (obj.HasKey(L"backgroundTasks") && obj.GetNamedValue(L"backgroundTasks").ValueType() == JsonValueType::Array)
            {
                for (const auto& value : obj.GetNamedArray(L"backgroundTasks"))
                {
                    if (value.ValueType() != JsonValueType::Object)
                    {
                        continue;
                    }
                    // Array elements are JsonValue instances, not JsonObject, so
                    // as<JsonObject>() fails; GetObject() collides with a Win32 macro.
                    const auto task = JsonObject::Parse(value.Stringify());
                    std::wstring type, description;
                    if (task.HasKey(L"type") && task.GetNamedValue(L"type").ValueType() == JsonValueType::String)
                    {
                        type = task.GetNamedString(L"type");
                    }
                    if (task.HasKey(L"description") && task.GetNamedValue(L"description").ValueType() == JsonValueType::String)
                    {
                        description = task.GetNamedString(L"description");
                    }
                    status.backgroundTasks.emplace_back(type.empty() ? description : type + L": " + description);
                }
            }

            status.present = !status.state.empty();
        }
        CATCH_LOG();
        return status;
    }

    Brush _solid(const Color& color)
    {
        return SolidColorBrush{ color };
    }

    Color _rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF)
    {
        return Color{ a, r, g, b };
    }

    Color _withAlpha(Color color, uint8_t alpha)
    {
        color.A = alpha;
        return color;
    }

    Palette _makePalette(const Brush& titlebarBrush)
    {
        Palette p;
        p.background = _rgba(0x20, 0x20, 0x20);
        if (const auto solid = titlebarBrush.try_as<SolidColorBrush>())
        {
            p.background = solid.Color();
        }
        else if (const auto acrylic = titlebarBrush.try_as<AcrylicBrush>())
        {
            p.background = acrylic.TintColor();
        }

        const auto luminance = 0.299 * p.background.R + 0.587 * p.background.G + 0.114 * p.background.B;
        p.isLight = luminance > 140;

        const auto fg = p.isLight ? _rgba(0x1A, 0x1A, 0x1A) : _rgba(0xFF, 0xFF, 0xFF);
        p.foreground = _solid(fg);
        p.secondary = _solid(_withAlpha(fg, 0x9E));
        p.hover = _solid(_withAlpha(fg, 0x12));
        p.pressed = _solid(_withAlpha(fg, 0x0A));
        p.selected = _solid(_withAlpha(fg, 0x1C));
        p.separator = _solid(_withAlpha(fg, 0x1F));
        p.transparent = _solid(Colors::Transparent());

        auto accent = p.isLight ? _rgba(0x00, 0x5F, 0xB8) : _rgba(0x60, 0xCD, 0xFF);
        try
        {
            const auto key = winrt::box_value(p.isLight ? L"SystemAccentColorDark1" : L"SystemAccentColorLight2");
            const auto res = Application::Current().Resources();
            if (res.HasKey(key))
            {
                accent = winrt::unbox_value<Color>(res.Lookup(key));
            }
        }
        CATCH_LOG();
        p.accent = _solid(accent);
        p.accentSubtle = _solid(_withAlpha(accent, 0x33));

        p.waiting = _solid(p.isLight ? _rgba(0x9D, 0x5D, 0x00) : _rgba(0xFF, 0xC8, 0x3D));
        p.done = _solid(p.isLight ? _rgba(0x0F, 0x7B, 0x0F) : _rgba(0x6C, 0xCB, 0x5F));
        p.error = _solid(p.isLight ? _rgba(0xC4, 0x2B, 0x1C) : _rgba(0xFF, 0x99, 0xA4));
        return p;
    }

    // Lightweight-styles a button so its hover/pressed states match the sidebar
    // instead of the app's default button colors.
    void _styleButton(const Button& button, const Palette& p, const Brush& background)
    {
        const auto res = button.Resources();
        const auto set = [&](const wchar_t* key, const Brush& brush) {
            res.Insert(winrt::box_value(key), brush);
        };
        set(L"ButtonBackground", background);
        set(L"ButtonBackgroundPointerOver", p.hover);
        set(L"ButtonBackgroundPressed", p.pressed);
        set(L"ButtonForeground", p.foreground);
        set(L"ButtonForegroundPointerOver", p.foreground);
        set(L"ButtonForegroundPressed", p.foreground);
        set(L"ButtonBorderBrush", p.transparent);
        set(L"ButtonBorderBrushPointerOver", p.transparent);
        set(L"ButtonBorderBrushPressed", p.transparent);
        button.Background(background);
        button.Foreground(p.foreground);
        button.BorderThickness(ThicknessHelper::FromUniformLength(0));
        button.CornerRadius(CornerRadiusHelper::FromUniformRadius(4));
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    }

    FontIcon _icon(std::wstring_view glyph, double size, const Brush& brush)
    {
        FontIcon icon;
        icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ IconFont });
        icon.Glyph(winrt::hstring{ glyph });
        icon.FontSize(size);
        icon.Foreground(brush);
        return icon;
    }

    TextBlock _text(const std::wstring& value, double size, const Brush& brush)
    {
        TextBlock text;
        text.Text(winrt::hstring{ value });
        text.FontSize(size);
        text.Foreground(brush);
        text.TextTrimming(TextTrimming::CharacterEllipsis);
        text.TextWrapping(TextWrapping::NoWrap);
        text.VerticalAlignment(VerticalAlignment::Center);
        return text;
    }

    Shapes::Ellipse _dot(double size, const Brush& fill, const Brush& stroke = nullptr)
    {
        Shapes::Ellipse dot;
        dot.Width(size);
        dot.Height(size);
        if (stroke)
        {
            dot.Stroke(stroke);
            dot.StrokeThickness(1.5);
            dot.Fill(SolidColorBrush{ Colors::Transparent() });
        }
        else
        {
            dot.Fill(fill);
        }
        dot.VerticalAlignment(VerticalAlignment::Center);
        dot.HorizontalAlignment(HorizontalAlignment::Center);
        return dot;
    }

    Attention _attentionOf(const PaneEntry& e)
    {
        if (!e.claude.present)
        {
            return Attention::None;
        }
        if (e.claude.state == L"waiting")
        {
            return Attention::Waiting;
        }
        if (e.claude.state == L"error")
        {
            return Attention::Error;
        }
        if (e.claude.state == L"working")
        {
            return Attention::Working;
        }
        return e.unseen ? Attention::Unseen : Attention::None;
    }

    std::wstring _statusText(const PaneEntry& e)
    {
        const auto& c = e.claude;
        if (!c.present)
        {
            return {};
        }
        std::wstring text;
        if (c.state == L"waiting")
        {
            text = c.detail.empty() ? L"Needs your input" : c.detail;
        }
        else if (c.state == L"working")
        {
            text = c.detail.empty() ? L"Working\u2026" : c.detail;
            if (c.subagents > 0)
            {
                text += fmt::format(FMT_COMPILE(L" \u00B7 {} agent{}"), c.subagents, c.subagents == 1 ? L"" : L"s");
            }
        }
        else if (c.state == L"error")
        {
            text = c.detail.empty() ? L"Stopped with an error" : c.detail;
        }
        else
        {
            text = e.unseen ? L"Done \u2014 your turn" : L"Ready";
        }
        return text;
    }

    // The status indicator in front of a pane: a spinner while Claude works,
    // a dot for the other Claude states, or a terminal glyph otherwise.
    FrameworkElement _statusIndicator(const PaneEntry& e, const Palette& p, double size)
    {
        switch (_attentionOf(e))
        {
        case Attention::Working:
        {
            winrt::MUX::Controls::ProgressRing ring;
            ring.IsActive(true);
            ring.Width(size + 2);
            ring.Height(size + 2);
            ring.MinWidth(0);
            ring.MinHeight(0);
            ring.Foreground(p.accent);
            ring.VerticalAlignment(VerticalAlignment::Center);
            return ring;
        }
        case Attention::Waiting:
            return _dot(size - 2, p.waiting);
        case Attention::Error:
            return _dot(size - 2, p.error);
        case Attention::Unseen:
            return _dot(size - 2, p.done);
        default:
            break;
        }
        if (e.claude.present)
        {
            return _dot(size - 3, nullptr, p.done);
        }
        return _icon(GlyphTerminal, size, p.secondary);
    }
}

namespace winrt::TerminalApp::implementation
{
    struct TerminalPage::SidebarState
    {
        struct InputHook
        {
            TermControl::KeySent_revoker keySent;
            TermControl::CharSent_revoker charSent;
        };

        DispatcherTimer timer{ nullptr };
        bool collapsed{ false };
        double expandedWidth{ SidebarDefaultWidth };
        bool resizing{ false };

        std::wstring signature;
        std::filesystem::path statusDir;

        std::unordered_set<std::wstring> collapsedGroups;
        std::unordered_map<std::wstring, std::pair<GitInfo, ULONGLONG>> gitCache;
        std::unordered_map<std::wstring, InputHook> inputHooks;
        // Session -> timestamp of the "waiting" status the user answered by typing.
        std::unordered_map<std::wstring, int64_t> answeredTs;
        // Session -> timestamp of the newest status the user has looked at.
        std::unordered_map<std::wstring, int64_t> seenTs;
        std::unordered_map<std::wstring, ClaudeStatus> lastStatus;
    };

    bool TerminalPage::_SidebarEnabled() const
    {
        return _settings && _settings.GlobalSettings().ShowTabsInSidebar();
    }

    void TerminalPage::_InitializeSidebar()
    {
        _sidebar = std::make_shared<SidebarState>();
        // .c_str(): the expanded string may carry its null terminator inside it.
        const auto profileDir = wil::ExpandEnvironmentStringsW<std::wstring>(L"%USERPROFILE%");
        _sidebar->statusDir = std::filesystem::path{ profileDir.c_str() } / L".claude" / L"terminal-status";

        // The tab strip's list (and its scroll buttons) stays in the TabView's
        // template; hide it once the template exists.
        _tabView.Loaded([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_SidebarUpdateTabStrip();
            }
        });

        const auto weak = get_weak();

        SidebarToggleButton().Click([weak](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_ToggleSidebarCollapsed();
            }
        });
        SidebarNewTabButton().Click([weak](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_OpenNewTerminalViaDropdown(NewTerminalArgs{});
            }
        });

        // Drag the right edge of the sidebar to resize it.
        const auto handle = SidebarResizeHandle();
        handle.PointerPressed([weak](const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            const auto page = weak.get();
            if (!page || !page->_sidebar || page->_sidebar->collapsed)
            {
                return;
            }
            if (sender.as<UIElement>().CapturePointer(e.Pointer()))
            {
                page->_sidebar->resizing = true;
                e.Handled(true);
            }
        });
        handle.PointerMoved([weak](auto&&, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            const auto page = weak.get();
            if (!page || !page->_sidebar || !page->_sidebar->resizing)
            {
                return;
            }
            const auto x = e.GetCurrentPoint(page->ContentRoot()).Position().X;
            const auto width = std::clamp(static_cast<double>(x), SidebarMinWidth, SidebarMaxWidth);
            page->_sidebar->expandedWidth = width;
            page->SidebarRoot().Width(width);
            e.Handled(true);
        });
        const auto endResize = [weak](const IInspectable& sender, const winrt::Windows::UI::Xaml::Input::PointerRoutedEventArgs& e) {
            const auto page = weak.get();
            if (!page || !page->_sidebar || !page->_sidebar->resizing)
            {
                return;
            }
            page->_sidebar->resizing = false;
            sender.as<UIElement>().ReleasePointerCapture(e.Pointer());
            e.Handled(true);
        };
        handle.PointerReleased(endResize);
        handle.PointerCaptureLost([weak](auto&&, auto&&) {
            if (const auto page = weak.get(); page && page->_sidebar)
            {
                page->_sidebar->resizing = false;
            }
        });

        // Claude Code status and the shells' working directories don't raise
        // events we can listen to, so poll. Rebuilding is skipped unless
        // something visible changed.
        _sidebar->timer = DispatcherTimer{};
        _sidebar->timer.Interval(SidebarPollInterval);
        _sidebar->timer.Tick([weak](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_RefreshSidebar(false);
            }
        });
        _sidebar->timer.Start();

        _UpdateSidebarVisibility();
    }

    void TerminalPage::_SidebarUpdateTabStrip()
    {
        if (!_tabView)
        {
            return;
        }
        // Breadth-first search for the TabView's "TabListView" template part.
        std::deque<DependencyObject> queue{ _tabView };
        while (!queue.empty())
        {
            const auto node = queue.front();
            queue.pop_front();
            if (const auto element = node.try_as<FrameworkElement>(); element && element.Name() == L"TabListView")
            {
                element.Visibility(_SidebarEnabled() ? Visibility::Collapsed : Visibility::Visible);
                return;
            }
            const auto count = winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(node);
            for (int32_t i = 0; i < count; ++i)
            {
                queue.push_back(winrt::Windows::UI::Xaml::Media::VisualTreeHelper::GetChild(node, i));
            }
        }
    }

    void TerminalPage::_UpdateSidebarVisibility()
    {
        const auto root = SidebarRoot();
        if (!root)
        {
            return;
        }
        const auto visible = _sidebar &&
                             _SidebarEnabled() &&
                             !_isInFocusMode &&
                             (!_isFullscreen || _showTabsFullscreen);
        root.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (visible)
        {
            _RefreshSidebar(false);
        }
    }

    bool TerminalPage::_ToggleSidebarCollapsed()
    {
        if (!_sidebar || !_SidebarEnabled())
        {
            return false;
        }
        _sidebar->collapsed = !_sidebar->collapsed;
        _RefreshSidebar(true);
        return true;
    }

    void TerminalPage::_SidebarActivatePane(const winrt::TerminalApp::Tab& tab, const uint32_t paneId)
    {
        uint32_t index{};
        if (!tab || !_tabs.IndexOf(tab, index))
        {
            return;
        }

        if (const auto tabImpl = _GetTabImpl(tab))
        {
            const auto alreadyFocused = _GetFocusedTabImpl() == tabImpl;
            const auto activePane = tabImpl->GetActivePane();
            const auto activeId = activePane ? activePane->Id() : std::nullopt;
            if (alreadyFocused && tabImpl->IsZoomed() && activeId != paneId)
            {
                _UnZoomIfNeeded();
            }
            tabImpl->FocusPane(paneId);
            if (alreadyFocused)
            {
                tabImpl->Focus(FocusState::Programmatic);
            }
        }

        _SelectTab(index);
        _RefreshSidebar(true);
    }

    // Closes the given panes. Tabs whose every pane is listed are closed as a
    // whole, which asks for confirmation the same way closing a tab does.
    void TerminalPage::_SidebarClosePanes(const std::vector<std::pair<winrt::TerminalApp::Tab, uint32_t>>& panes)
    {
        std::vector<winrt::TerminalApp::Tab> wholeTabs;
        std::vector<std::pair<winrt::TerminalApp::Tab, std::vector<uint32_t>>> partialTabs;
        for (const auto& [tab, unused] : panes)
        {
            const auto alreadySeen = std::any_of(wholeTabs.begin(), wholeTabs.end(), [&](const auto& t) { return t == tab; }) ||
                                     std::any_of(partialTabs.begin(), partialTabs.end(), [&](const auto& p) { return p.first == tab; });
            const auto tabImpl = _GetTabImpl(tab);
            if (alreadySeen || !tabImpl)
            {
                continue;
            }
            std::vector<uint32_t> ids;
            for (const auto& [otherTab, paneId] : panes)
            {
                if (otherTab == tab)
                {
                    ids.push_back(paneId);
                }
            }
            if (ids.size() >= gsl::narrow_cast<size_t>(tabImpl->GetLeafPaneCount()))
            {
                wholeTabs.push_back(tab);
            }
            else
            {
                partialTabs.emplace_back(tab, std::move(ids));
            }
        }

        for (auto& [tab, ids] : partialTabs)
        {
            if (const auto tabImpl = _GetTabImpl(tab))
            {
                _ClosePanes(tabImpl->get_weak(), std::move(ids));
            }
        }
        if (!wholeTabs.empty())
        {
            _RemoveTabs(std::move(wholeTabs));
        }
    }

    void TerminalPage::_SidebarOpenTabIn(const winrt::hstring& directory)
    {
        NewTerminalArgs args;
        args.StartingDirectory(directory);
        _OpenNewTab(args);
    }

    void TerminalPage::_SidebarOnPaneInput(const std::wstring& sessionId)
    {
        if (!_sidebar)
        {
            return;
        }
        const auto it = _sidebar->lastStatus.find(sessionId);
        if (it != _sidebar->lastStatus.end() && it->second.state == L"waiting")
        {
            // The user answered the prompt. Claude Code doesn't report that,
            // so assume it's working until its next status update.
            _sidebar->answeredTs[sessionId] = it->second.ts;
            _RefreshSidebar(false);
        }
    }

    void TerminalPage::_SidebarHookInput(const TermControl& control, const std::wstring& sessionId)
    {
        if (!_sidebar || _sidebar->inputHooks.contains(sessionId))
        {
            return;
        }

        const auto weak = get_weak();
        SidebarState::InputHook hook;
        hook.keySent = control.KeySent(winrt::auto_revoke, [weak, sessionId](auto&&, const KeySentEventArgs& e) {
            if (e.KeyDown() && (e.VKey() == VK_RETURN || e.VKey() == VK_ESCAPE))
            {
                if (const auto page = weak.get())
                {
                    page->_SidebarOnPaneInput(sessionId);
                }
            }
        });
        hook.charSent = control.CharSent(winrt::auto_revoke, [weak, sessionId](auto&&, const CharSentEventArgs& e) {
            const auto ch = e.Character();
            if ((ch >= L'1' && ch <= L'9') || ch == L'\r' || ch == L'y' || ch == L'n' || ch == 0x1b)
            {
                if (const auto page = weak.get())
                {
                    page->_SidebarOnPaneInput(sessionId);
                }
            }
        });
        _sidebar->inputHooks.emplace(sessionId, std::move(hook));
    }

    void TerminalPage::_RefreshSidebar(const bool force)
    {
        if (!_sidebar || !_SidebarEnabled())
        {
            return;
        }
        const auto root = SidebarRoot();
        if (!root || root.Visibility() != Visibility::Visible)
        {
            return;
        }

        auto& state = *_sidebar;
        const auto now = GetTickCount64();
        const auto focusedTab = _GetFocusedTabImpl();

        // 1. Collect every pane of every tab.
        std::vector<PaneEntry> entries;
        std::unordered_set<std::wstring> liveSessions;
        for (const auto& tab : _tabs)
        {
            const auto tabImpl = _GetTabImpl(tab);
            if (!tabImpl)
            {
                continue;
            }
            const auto rootPane = tabImpl->GetRootPane();
            if (!rootPane)
            {
                continue;
            }
            const auto activePane = tabImpl->GetActivePane();
            const auto tabFocused = focusedTab == tabImpl;
            const auto singlePane = tabImpl->GetLeafPaneCount() == 1;

            rootPane->WalkTree([&](const std::shared_ptr<Pane>& pane) {
                const auto content = pane->GetContent();
                if (!content)
                {
                    return;
                }

                PaneEntry e;
                e.tab = tab;
                e.paneId = pane->Id().value_or(0);
                e.active = tabFocused && pane == activePane;
                // A single-pane tab uses the tab title, so renamed tabs show their name.
                e.title = singlePane ? std::wstring{ tabImpl->Title() } : std::wstring{ content.Title() };

                if (const auto terminal = content.try_as<winrt::TerminalApp::TerminalPaneContent>())
                {
                    if (const auto control = terminal.GetTermControl())
                    {
                        if (const auto connection = control.Connection())
                        {
                            if (const auto guid = connection.SessionId(); guid != winrt::guid{})
                            {
                                e.sessionId = _toLower(Utils::GuidToPlainString(guid));
                            }
                        }
                        e.cwd = _normalizePath(control.WorkingDirectory());
                        if (e.cwd.empty())
                        {
                            if (const auto profile = terminal.GetProfile())
                            {
                                e.cwd = _normalizePath(profile.EvaluatedStartingDirectory());
                            }
                        }
                        if (!e.sessionId.empty())
                        {
                            _SidebarHookInput(control, e.sessionId);
                        }
                    }
                }

                // Claude Code's view of the session wins: it knows its own cwd
                // even though the shell can't report it while Claude runs.
                if (!e.sessionId.empty())
                {
                    liveSessions.insert(e.sessionId);
                    auto status = _readClaudeStatus(state.statusDir / (e.sessionId + L".json"));
                    if (status.present && !_processAlive(status.pid))
                    {
                        status = {};
                    }
                    if (status.present)
                    {
                        if (status.state == L"waiting")
                        {
                            if (const auto it = state.answeredTs.find(e.sessionId); it != state.answeredTs.end() && it->second == status.ts)
                            {
                                status.state = L"working";
                                status.detail.clear();
                            }
                        }
                        if (!status.cwd.empty())
                        {
                            e.cwd = _normalizePath(status.cwd);
                        }

                        const auto seen = state.seenTs.try_emplace(e.sessionId, status.ts).first;
                        if (e.active && _activated)
                        {
                            seen->second = status.ts;
                        }
                        e.unseen = status.state == L"idle" && status.ts > seen->second;
                    }
                    state.lastStatus[e.sessionId] = status;
                    e.claude = std::move(status);
                }

                if (!e.cwd.empty())
                {
                    const auto key = _toLower(e.cwd);
                    auto it = state.gitCache.find(key);
                    if (it == state.gitCache.end() || now - it->second.second > GitCacheLifetimeMs)
                    {
                        it = state.gitCache.insert_or_assign(key, std::pair{ _resolveGit(e.cwd), now }).first;
                    }
                    e.git = it->second.first;
                }

                entries.emplace_back(std::move(e));
            });
        }

        // Forget about sessions that went away.
        std::erase_if(state.inputHooks, [&](const auto& kv) { return !liveSessions.contains(kv.first); });
        std::erase_if(state.answeredTs, [&](const auto& kv) { return !liveSessions.contains(kv.first); });
        std::erase_if(state.seenTs, [&](const auto& kv) { return !liveSessions.contains(kv.first); });
        std::erase_if(state.lastStatus, [&](const auto& kv) { return !liveSessions.contains(kv.first); });
        if (state.gitCache.size() > 256)
        {
            state.gitCache.clear();
        }

        // 2. Group by repository, then worktree.
        std::vector<RepoGroup> groups;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            std::wstring key, name, groupRoot, worktreeKey, worktreeRoot;
            auto isGit = false;
            if (e.git.isGit)
            {
                isGit = true;
                key = L"git:" + _toLower(e.git.repoRoot);
                name = _leafName(e.git.repoRoot);
                groupRoot = e.git.repoRoot;
                worktreeKey = _toLower(e.git.worktreeRoot);
                worktreeRoot = e.git.worktreeRoot;
            }
            else if (!e.cwd.empty())
            {
                key = L"dir:" + _toLower(e.cwd);
                name = _leafName(e.cwd);
                groupRoot = e.cwd;
            }
            else
            {
                key = L"other";
                name = L"Other";
            }

            auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.key == key; });
            if (group == groups.end())
            {
                groups.push_back(RepoGroup{ key, name.empty() ? groupRoot : name, groupRoot, isGit, {} });
                group = std::prev(groups.end());
            }

            auto worktree = std::find_if(group->worktrees.begin(), group->worktrees.end(), [&](const auto& w) { return w.key == worktreeKey; });
            if (worktree == group->worktrees.end())
            {
                group->worktrees.push_back(WorktreeGroup{ worktreeKey, worktreeRoot, e.git.branch, isGit && _toLower(worktreeRoot) == _toLower(groupRoot), {} });
                worktree = std::prev(group->worktrees.end());
            }
            worktree->entries.push_back(i);
        }
        for (auto& group : groups)
        {
            std::stable_sort(group.worktrees.begin(), group.worktrees.end(), [](const auto& a, const auto& b) { return a.isMain && !b.isMain; });
        }

        // 3. Skip the rebuild if nothing visible changed.
        const auto palette = _makePalette(TitlebarBrush());
        std::wstring signature;
        signature.reserve(1024);
        signature += state.collapsed ? L"C|" : L"E|";
        signature += fmt::format(FMT_COMPILE(L"{:02x}{:02x}{:02x}{:02x}|"), palette.background.A, palette.background.R, palette.background.G, palette.background.B);
        for (const auto& group : groups)
        {
            signature += group.key;
            signature += state.collapsedGroups.contains(group.key) ? L"|-|" : L"|+|";
            for (const auto& worktree : group.worktrees)
            {
                signature += worktree.key + L"|" + worktree.branch + L"|";
                for (const auto i : worktree.entries)
                {
                    const auto& e = entries[i];
                    signature += fmt::format(FMT_COMPILE(L"{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|"),
                                             e.paneId,
                                             e.active,
                                             e.title,
                                             e.cwd,
                                             e.claude.state,
                                             e.claude.detail,
                                             e.claude.subagents,
                                             e.claude.backgroundTasks.size(),
                                             e.unseen,
                                             reinterpret_cast<uintptr_t>(winrt::get_abi(e.tab)));
                    for (const auto& task : e.claude.backgroundTasks)
                    {
                        signature += task + L"|";
                    }
                }
            }
        }
        if (!force && signature == state.signature)
        {
            return;
        }
        state.signature = std::move(signature);

        // 4. Rebuild.
        root.Width(state.collapsed ? SidebarCompactWidth : state.expandedWidth);
        root.Background(TitlebarBrush());
        root.BorderBrush(palette.separator);
        root.BorderThickness(ThicknessHelper::FromLengths(0, 0, 1, 0));

        SidebarTitle().Visibility(state.collapsed ? Visibility::Collapsed : Visibility::Visible);
        SidebarTitle().Foreground(palette.foreground);
        SidebarNewTabButton().Visibility(state.collapsed ? Visibility::Collapsed : Visibility::Visible);
        _styleButton(SidebarToggleButton(), palette, palette.transparent);
        _styleButton(SidebarNewTabButton(), palette, palette.transparent);
        SidebarToggleButton().HorizontalAlignment(state.collapsed ? HorizontalAlignment::Center : HorizontalAlignment::Left);
        SidebarToggleButton().Width(36);
        SidebarNewTabButton().Width(32);
        if (const auto icon = SidebarToggleButton().Content().try_as<FontIcon>())
        {
            icon.Foreground(palette.foreground);
        }
        if (const auto icon = SidebarNewTabButton().Content().try_as<FontIcon>())
        {
            icon.Foreground(palette.foreground);
        }

        const auto items = SidebarItems();
        items.Children().Clear();
        const auto weak = get_weak();

        const auto makeActivateHandler = [weak](const PaneEntry& e) {
            return [weak, tab = winrt::make_weak(e.tab), paneId = e.paneId](auto&&, auto&&) {
                const auto page = weak.get();
                const auto strongTab = tab.get();
                if (page && strongTab)
                {
                    page->_SidebarActivatePane(strongTab, paneId);
                }
            };
        };

        const auto makeContextMenu = [weak](const PaneEntry& e) {
            MenuFlyout menu;
            const auto addItem = [&](const wchar_t* text, std::wstring_view glyph, auto&& handler) {
                MenuFlyoutItem item;
                item.Text(text);
                FontIcon icon;
                icon.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily{ IconFont });
                icon.Glyph(winrt::hstring{ glyph });
                item.Icon(icon);
                item.Click(handler);
                menu.Items().Append(item);
            };
            const auto cwd = winrt::hstring{ e.cwd };
            if (!e.cwd.empty())
            {
                addItem(L"New tab here", GlyphAdd, [weak, cwd](auto&&, auto&&) {
                    if (const auto page = weak.get())
                    {
                        page->_SidebarOpenTabIn(cwd);
                    }
                });
                addItem(L"Copy path", L"\xE8C8", [cwd](auto&&, auto&&) {
                    winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
                    package.SetText(cwd);
                    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
                });
            }
            addItem(L"Close", L"\xE711", [weak, tab = e.tab, paneId = e.paneId](auto&&, auto&&) {
                if (const auto page = weak.get())
                {
                    page->_SidebarClosePanes({ { tab, paneId } });
                }
            });
            return menu;
        };

        const auto makeTooltip = [](const PaneEntry& e, const std::wstring& groupName) {
            std::wstring tip = e.title;
            if (!groupName.empty())
            {
                tip += L"\n" + groupName;
                if (e.git.isGit && !e.git.branch.empty())
                {
                    tip += L" \u00B7 " + e.git.branch;
                }
            }
            if (!e.cwd.empty())
            {
                tip += L"\n" + e.cwd;
            }
            if (e.claude.present)
            {
                tip += L"\n\nClaude Code: " + _statusText(e);
                if (!e.claude.backgroundTasks.empty())
                {
                    tip += fmt::format(FMT_COMPILE(L"\nBackground tasks ({}):"), e.claude.backgroundTasks.size());
                    for (const auto& task : e.claude.backgroundTasks)
                    {
                        tip += L"\n  \u2022 " + task;
                    }
                }
            }
            return tip;
        };

        if (state.collapsed)
        {
            // Compact rail: one square per pane, labeled with its group's initial.
            auto first = true;
            for (const auto& group : groups)
            {
                if (!first)
                {
                    Border separator;
                    separator.Height(1);
                    separator.Margin(ThicknessHelper::FromLengths(8, 4, 8, 4));
                    separator.Background(palette.separator);
                    items.Children().Append(separator);
                }
                first = false;

                for (const auto& worktree : group.worktrees)
                {
                    for (const auto i : worktree.entries)
                    {
                        const auto& e = entries[i];
                        Button button;
                        _styleButton(button, palette, e.active ? palette.selected : palette.transparent);
                        button.Width(38);
                        button.Height(38);
                        button.Padding(ThicknessHelper::FromUniformLength(0));
                        button.Margin(ThicknessHelper::FromLengths(0, 1, 0, 1));
                        button.HorizontalAlignment(HorizontalAlignment::Center);

                        Grid content;
                        content.Width(38);
                        content.Height(38);
                        const auto initial = group.name.empty() ? std::wstring{ L"?" } : std::wstring{ static_cast<wchar_t>(towupper(group.name[0])) };
                        auto letter = _text(initial, 14, e.active ? palette.foreground : palette.secondary);
                        letter.FontWeight(Text::FontWeights::SemiBold());
                        letter.HorizontalAlignment(HorizontalAlignment::Center);
                        content.Children().Append(letter);

                        if (_attentionOf(e) != Attention::None || e.claude.present)
                        {
                            auto indicator = _statusIndicator(e, palette, 10);
                            indicator.HorizontalAlignment(HorizontalAlignment::Right);
                            indicator.VerticalAlignment(VerticalAlignment::Bottom);
                            indicator.Margin(ThicknessHelper::FromLengths(0, 0, 4, 4));
                            content.Children().Append(indicator);
                        }
                        if (!e.claude.backgroundTasks.empty())
                        {
                            auto count = _text(std::to_wstring(e.claude.backgroundTasks.size()), 9, palette.accent);
                            count.HorizontalAlignment(HorizontalAlignment::Right);
                            count.VerticalAlignment(VerticalAlignment::Top);
                            count.Margin(ThicknessHelper::FromLengths(0, 2, 4, 0));
                            content.Children().Append(count);
                        }
                        if (e.active)
                        {
                            Border bar;
                            bar.Width(3);
                            bar.Height(16);
                            bar.CornerRadius(CornerRadiusHelper::FromUniformRadius(1.5));
                            bar.Background(palette.accent);
                            bar.HorizontalAlignment(HorizontalAlignment::Left);
                            bar.VerticalAlignment(VerticalAlignment::Center);
                            content.Children().Append(bar);
                        }

                        button.Content(content);
                        button.Click(makeActivateHandler(e));
                        button.ContextFlyout(makeContextMenu(e));
                        ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring{ makeTooltip(e, group.name) }));
                        Automation::AutomationProperties::SetName(button, winrt::hstring{ e.title });
                        items.Children().Append(button);
                    }
                }
            }
            return;
        }

        for (const auto& group : groups)
        {
            const auto groupCollapsed = state.collapsedGroups.contains(group.key);

            auto attention = Attention::None;
            size_t paneCount = 0;
            for (const auto& worktree : group.worktrees)
            {
                for (const auto i : worktree.entries)
                {
                    attention = std::max(attention, _attentionOf(entries[i]));
                    ++paneCount;
                }
            }

            // Group header: chevron, name, attention dot, pane count.
            {
                Button header;
                _styleButton(header, palette, palette.transparent);
                header.Padding(ThicknessHelper::FromLengths(4, 6, 8, 6));
                header.Margin(ThicknessHelper::FromLengths(0, 6, 0, 0));

                Grid grid;
                const auto addColumn = [&](GridLength length) {
                    ColumnDefinition column;
                    column.Width(length);
                    grid.ColumnDefinitions().Append(column);
                };
                addColumn(GridLengthHelper::FromPixels(18));
                addColumn(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                addColumn(GridLengthHelper::Auto());
                addColumn(GridLengthHelper::Auto());

                auto chevron = _icon(groupCollapsed ? GlyphChevronRight : GlyphChevronDown, 9, palette.secondary);
                Grid::SetColumn(chevron, 0);
                grid.Children().Append(chevron);

                auto name = _text(group.name, 12, palette.foreground);
                name.FontWeight(Text::FontWeights::SemiBold());
                name.Margin(ThicknessHelper::FromLengths(2, 0, 6, 0));
                Grid::SetColumn(name, 1);
                grid.Children().Append(name);

                if (attention == Attention::Waiting || attention == Attention::Error || (groupCollapsed && attention != Attention::None))
                {
                    const auto& brush = attention == Attention::Waiting ? palette.waiting :
                                        attention == Attention::Error   ? palette.error :
                                        attention == Attention::Working ? palette.accent :
                                                                          palette.done;
                    auto dot = _dot(7, brush);
                    dot.Margin(ThicknessHelper::FromLengths(0, 0, 6, 0));
                    Grid::SetColumn(dot, 2);
                    grid.Children().Append(dot);
                }

                auto count = _text(std::to_wstring(paneCount), 11, palette.secondary);
                Grid::SetColumn(count, 3);
                grid.Children().Append(count);

                header.Content(grid);
                ToolTipService::SetToolTip(header, winrt::box_value(winrt::hstring{ group.root.empty() ? group.name : group.root }));
                Automation::AutomationProperties::SetName(header, winrt::hstring{ group.name });
                header.Click([weak, key = group.key](auto&&, auto&&) {
                    const auto page = weak.get();
                    if (!page || !page->_sidebar)
                    {
                        return;
                    }
                    auto& collapsedGroups = page->_sidebar->collapsedGroups;
                    if (!collapsedGroups.erase(key))
                    {
                        collapsedGroups.insert(key);
                    }
                    page->_RefreshSidebar(true);
                });
                {
                    MenuFlyout menu;
                    if (!group.root.empty())
                    {
                        MenuFlyoutItem item;
                        item.Text(L"New tab here");
                        item.Click([weak, dir = winrt::hstring{ group.root }](auto&&, auto&&) {
                            if (const auto page = weak.get())
                            {
                                page->_SidebarOpenTabIn(dir);
                            }
                        });
                        menu.Items().Append(item);
                    }

                    // A group only exists while terminals are open in it, so
                    // removing a group means closing its terminals.
                    std::vector<std::pair<winrt::TerminalApp::Tab, uint32_t>> panes;
                    for (const auto& worktree : group.worktrees)
                    {
                        for (const auto i : worktree.entries)
                        {
                            panes.emplace_back(entries[i].tab, entries[i].paneId);
                        }
                    }
                    MenuFlyoutItem close;
                    close.Text(winrt::hstring{ paneCount == 1 ? std::wstring{ L"Close group (1 terminal)" } : fmt::format(FMT_COMPILE(L"Close group ({} terminals)"), paneCount) });
                    close.Icon(_icon(L"\xE711", 14, palette.foreground));
                    close.Click([weak, panes](auto&&, auto&&) {
                        if (const auto page = weak.get())
                        {
                            page->_SidebarClosePanes(panes);
                        }
                    });
                    menu.Items().Append(close);
                    header.ContextFlyout(menu);
                }
                items.Children().Append(header);
            }

            if (groupCollapsed)
            {
                continue;
            }

            for (const auto& worktree : group.worktrees)
            {
                const auto indent = group.isGit ? 14.0 : 0.0;

                // Worktree header (git only): main worktree gets a home glyph,
                // linked worktrees a folder glyph and their folder name.
                if (group.isGit)
                {
                    Grid row;
                    row.Margin(ThicknessHelper::FromLengths(10, 4, 0, 1));
                    const auto addColumn = [&](GridLength length) {
                        ColumnDefinition column;
                        column.Width(length);
                        row.ColumnDefinitions().Append(column);
                    };
                    addColumn(GridLengthHelper::FromPixels(18));
                    addColumn(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                    addColumn(GridLengthHelper::Auto());

                    auto glyph = _icon(worktree.isMain ? GlyphHome : GlyphFolder, 11, palette.secondary);
                    Grid::SetColumn(glyph, 0);
                    row.Children().Append(glyph);

                    // "<branch>  <folder>", the folder only when the branch name
                    // doesn't already say it (worktree-foo in folder foo).
                    const auto folder = _leafName(worktree.root);
                    const auto branchName = worktree.branch.empty() ? folder : worktree.branch;
                    auto label = _text(L"", 12, palette.foreground);
                    winrt::Windows::UI::Xaml::Documents::Run branchRun;
                    branchRun.Text(winrt::hstring{ branchName });
                    label.Inlines().Append(branchRun);
                    if (!worktree.isMain && !folder.empty() && _toLower(branchName).find(_toLower(folder)) == std::wstring::npos)
                    {
                        winrt::Windows::UI::Xaml::Documents::Run folderRun;
                        folderRun.Text(winrt::hstring{ L"  " + folder });
                        folderRun.FontSize(11);
                        folderRun.Foreground(palette.secondary);
                        label.Inlines().Append(folderRun);
                    }
                    Grid::SetColumn(label, 1);
                    row.Children().Append(label);

                    Button add;
                    _styleButton(add, palette, palette.transparent);
                    add.Width(24);
                    add.Height(22);
                    add.Padding(ThicknessHelper::FromUniformLength(0));
                    add.HorizontalAlignment(HorizontalAlignment::Right);
                    add.Content(_icon(GlyphAdd, 10, palette.secondary));
                    ToolTipService::SetToolTip(add, winrt::box_value(winrt::hstring{ L"New tab in " + worktree.root }));
                    Automation::AutomationProperties::SetName(add, L"New tab in this worktree");
                    add.Click([weak, dir = winrt::hstring{ worktree.root }](auto&&, auto&&) {
                        if (const auto page = weak.get())
                        {
                            page->_SidebarOpenTabIn(dir);
                        }
                    });
                    Grid::SetColumn(add, 2);
                    row.Children().Append(add);

                    ToolTipService::SetToolTip(row, winrt::box_value(winrt::hstring{ worktree.root }));
                    items.Children().Append(row);
                }

                for (const auto i : worktree.entries)
                {
                    const auto& e = entries[i];

                    Grid outer;
                    outer.Margin(ThicknessHelper::FromLengths(indent, 1, 0, 1));

                    Button button;
                    _styleButton(button, palette, e.active ? palette.selected : palette.transparent);
                    button.Padding(ThicknessHelper::FromLengths(8, 5, 6, 5));

                    Grid grid;
                    const auto addColumn = [&](GridLength length) {
                        ColumnDefinition column;
                        column.Width(length);
                        grid.ColumnDefinitions().Append(column);
                    };
                    addColumn(GridLengthHelper::FromPixels(20));
                    addColumn(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
                    addColumn(GridLengthHelper::Auto());

                    auto indicator = _statusIndicator(e, palette, 12);
                    indicator.HorizontalAlignment(HorizontalAlignment::Left);
                    Grid::SetColumn(indicator, 0);
                    grid.Children().Append(indicator);

                    StackPanel text;
                    text.VerticalAlignment(VerticalAlignment::Center);
                    auto title = _text(e.title.empty() ? std::wstring{ L"Terminal" } : e.title, 13, palette.foreground);
                    text.Children().Append(title);

                    // Second line: Claude's status, or the subdirectory the
                    // shell is in relative to the worktree.
                    std::wstring subtitle = _statusText(e);
                    const Brush* subtitleBrush = &palette.secondary;
                    switch (_attentionOf(e))
                    {
                    case Attention::Waiting:
                        subtitleBrush = &palette.waiting;
                        break;
                    case Attention::Error:
                        subtitleBrush = &palette.error;
                        break;
                    case Attention::Unseen:
                        subtitleBrush = &palette.done;
                        break;
                    default:
                        break;
                    }
                    if (subtitle.empty() && !e.cwd.empty())
                    {
                        const auto& base = e.git.isGit ? e.git.worktreeRoot : std::wstring{};
                        if (!base.empty() && e.cwd.size() > base.size() + 1 && _toLower(e.cwd).starts_with(_toLower(base)))
                        {
                            subtitle = e.cwd.substr(base.size() + 1);
                        }
                    }
                    if (!subtitle.empty())
                    {
                        auto second = _text(subtitle, 11, *subtitleBrush);
                        text.Children().Append(second);
                    }
                    Grid::SetColumn(text, 1);
                    grid.Children().Append(text);

                    if (!e.claude.backgroundTasks.empty())
                    {
                        Border badge;
                        badge.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
                        badge.Padding(ThicknessHelper::FromLengths(5, 1, 6, 1));
                        badge.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
                        badge.Background(palette.accentSubtle);
                        badge.VerticalAlignment(VerticalAlignment::Center);
                        StackPanel badgeContent;
                        badgeContent.Orientation(Orientation::Horizontal);
                        auto clock = _icon(GlyphStopwatch, 10, palette.accent);
                        clock.Margin(ThicknessHelper::FromLengths(0, 0, 3, 0));
                        badgeContent.Children().Append(clock);
                        badgeContent.Children().Append(_text(std::to_wstring(e.claude.backgroundTasks.size()), 11, palette.accent));
                        badge.Child(badgeContent);
                        Grid::SetColumn(badge, 2);
                        grid.Children().Append(badge);
                    }

                    button.Content(grid);
                    button.Click(makeActivateHandler(e));
                    button.ContextFlyout(makeContextMenu(e));
                    ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring{ makeTooltip(e, group.name) }));
                    Automation::AutomationProperties::SetName(button, winrt::hstring{ e.title + L" " + subtitle });
                    outer.Children().Append(button);

                    if (e.active)
                    {
                        Border bar;
                        bar.Width(3);
                        bar.Height(16);
                        bar.CornerRadius(CornerRadiusHelper::FromUniformRadius(1.5));
                        bar.Background(palette.accent);
                        bar.HorizontalAlignment(HorizontalAlignment::Left);
                        bar.VerticalAlignment(VerticalAlignment::Center);
                        bar.IsHitTestVisible(false);
                        outer.Children().Append(bar);
                    }

                    items.Children().Append(outer);
                }
            }
        }
    }
}
