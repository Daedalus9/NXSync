#include "nxsync/gui.hpp"

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace nxsync {
namespace {

constexpr int ScreenWidth = 1280;
constexpr int ScreenHeight = 720;

constexpr SDL_Color Background{14, 20, 31, 255};
constexpr SDL_Color Panel{25, 34, 49, 255};
constexpr SDL_Color PanelRaised{33, 44, 62, 255};
constexpr SDL_Color Border{58, 72, 94, 255};
constexpr SDL_Color Text{241, 245, 249, 255};
constexpr SDL_Color TextMuted{154, 168, 188, 255};
constexpr SDL_Color Accent{45, 212, 191, 255};
constexpr SDL_Color AccentDark{17, 94, 89, 255};
constexpr SDL_Color Good{74, 222, 128, 255};
constexpr SDL_Color Warning{251, 191, 36, 255};
constexpr SDL_Color Danger{248, 113, 113, 255};
constexpr SDL_Color Blue{96, 165, 250, 255};

SDL_Color saveStateColor(const GuiSaveState state) {
    switch (state) {
        case GuiSaveState::CloudCurrent:
            return Good;
        case GuiSaveState::LocalCurrent:
            return Blue;
        case GuiSaveState::Changed:
            return Warning;
        case GuiSaveState::UploadPending:
            return SDL_Color{192, 132, 252, 255};
        case GuiSaveState::Error:
            return Danger;
        case GuiSaveState::NotBackedUp:
        case GuiSaveState::Unknown:
        default:
            return TextMuted;
    }
}

SDL_Color toneColor(const GuiTone tone) {
    switch (tone) {
        case GuiTone::Neutral: return TextMuted;
        case GuiTone::Good: return Good;
        case GuiTone::Warning: return Warning;
        case GuiTone::Danger: return Danger;
        case GuiTone::Accent:
        default: return Accent;
    }
}

void setColor(SDL_Renderer* renderer, const SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void fillRect(SDL_Renderer* renderer, const SDL_Rect& rect, const SDL_Color color) {
    setColor(renderer, color);
    SDL_RenderFillRect(renderer, &rect);
}

void outlineRect(SDL_Renderer* renderer, const SDL_Rect& rect, const SDL_Color color) {
    setColor(renderer, color);
    SDL_RenderDrawRect(renderer, &rect);
}

std::string removeLastUtf8Codepoint(std::string value) {
    if (value.empty()) {
        return value;
    }
    std::size_t index = value.size() - 1;
    while (index > 0
        && (static_cast<unsigned char>(value[index]) & 0xC0U) == 0x80U) {
        --index;
    }
    value.erase(index);
    return value;
}

} // namespace

struct Gui::Impl {
    SDL_Window* window{nullptr};
    SDL_Renderer* renderer{nullptr};
    TTF_Font* fontSmall{nullptr};
    TTF_Font* fontBody{nullptr};
    TTF_Font* fontMedium{nullptr};
    TTF_Font* fontTitle{nullptr};
    bool sdlInitialized{false};
    bool ttfInitialized{false};
    bool fontServiceInitialized{false};
    bool initialized{false};
    std::string lastError;

    TTF_Font* openSystemFont(const PlFontData& fontData, const int pointSize) {
        SDL_RWops* source = SDL_RWFromConstMem(fontData.address, fontData.size);
        if (source == nullptr) {
            return nullptr;
        }
        return TTF_OpenFontRW(source, 1, pointSize);
    }

    void text(
        TTF_Font* font,
        const std::string& value,
        const int x,
        const int y,
        const SDL_Color color,
        const int maxWidth = 0) {
        if (font == nullptr || value.empty()) {
            return;
        }

        std::string renderedValue = value;
        if (maxWidth > 0) {
            int width = 0;
            int height = 0;
            TTF_SizeUTF8(font, renderedValue.c_str(), &width, &height);
            if (width > maxWidth) {
                const std::string suffix = "...";
                while (!renderedValue.empty()) {
                    renderedValue = removeLastUtf8Codepoint(renderedValue);
                    const std::string candidate = renderedValue + suffix;
                    TTF_SizeUTF8(font, candidate.c_str(), &width, &height);
                    if (width <= maxWidth) {
                        renderedValue = candidate;
                        break;
                    }
                }
            }
        }

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, renderedValue.c_str(), color);
        if (surface == nullptr) {
            return;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (texture != nullptr) {
            const SDL_Rect destination{x, y, surface->w, surface->h};
            SDL_RenderCopy(renderer, texture, nullptr, &destination);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }

    void buttonHint(
        const int x,
        const int y,
        const char* button,
        const char* label,
        const SDL_Color color) {
        SDL_Rect badge{x, y, 32, 32};
        fillRect(renderer, badge, color);
        outlineRect(renderer, badge, Text);
        int buttonWidth = 0;
        int buttonHeight = 0;
        TTF_SizeUTF8(fontSmall, button, &buttonWidth, &buttonHeight);
        text(fontSmall, button, x + (32 - buttonWidth) / 2, y + 5, Text);
        text(fontSmall, label, x + 42, y + 5, TextMuted);
    }

    void beginFrame() {
        SDL_PumpEvents();
        setColor(renderer, Background);
        SDL_RenderClear(renderer);
    }

    void header(const std::string& version, const std::string& device) {
        const SDL_Rect headerRect{0, 0, ScreenWidth, 88};
        fillRect(renderer, headerRect, Panel);
        const SDL_Rect accentLine{0, 86, ScreenWidth, 2};
        fillRect(renderer, accentLine, Accent);
        text(fontTitle, "NXSync", 28, 8, Text);
        text(fontSmall, version, 30, 57, TextMuted);
        if (!device.empty()) {
            text(fontSmall, "Console", 948, 18, TextMuted);
            text(fontMedium, device, 948, 40, Text, 300);
        }
    }

    void present() {
        SDL_RenderPresent(renderer);
    }
};

Gui::Gui()
    : impl_(new Impl()) {}

Gui::~Gui() {
    shutdown();
}

bool Gui::initialize() {
    shutdown();
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        impl_->lastError = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    impl_->sdlInitialized = true;

    impl_->window = SDL_CreateWindow(
        "NXSync",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        ScreenWidth,
        ScreenHeight,
        SDL_WINDOW_SHOWN);
    if (impl_->window == nullptr) {
        impl_->lastError = std::string("SDL_CreateWindow: ") + SDL_GetError();
        shutdown();
        return false;
    }
    impl_->renderer = SDL_CreateRenderer(
        impl_->window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (impl_->renderer == nullptr) {
        impl_->renderer = SDL_CreateRenderer(impl_->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (impl_->renderer == nullptr) {
        impl_->lastError = std::string("SDL_CreateRenderer: ") + SDL_GetError();
        shutdown();
        return false;
    }
    SDL_SetRenderDrawBlendMode(impl_->renderer, SDL_BLENDMODE_BLEND);

    if (TTF_Init() != 0) {
        impl_->lastError = std::string("TTF_Init: ") + TTF_GetError();
        shutdown();
        return false;
    }
    impl_->ttfInitialized = true;

    const Result fontServiceResult = plInitialize(PlServiceType_User);
    if (R_FAILED(fontServiceResult)) {
        impl_->lastError = "Switch font service unavailable";
        shutdown();
        return false;
    }
    impl_->fontServiceInitialized = true;

    PlFontData fontData{};
    const Result fontResult = plGetSharedFontByType(
        &fontData,
        PlSharedFontType_Standard);
    if (R_FAILED(fontResult) || fontData.address == nullptr || fontData.size == 0) {
        impl_->lastError = "Switch system font unavailable";
        shutdown();
        return false;
    }

    impl_->fontSmall = impl_->openSystemFont(fontData, 17);
    impl_->fontBody = impl_->openSystemFont(fontData, 20);
    impl_->fontMedium = impl_->openSystemFont(fontData, 24);
    impl_->fontTitle = impl_->openSystemFont(fontData, 36);
    if (impl_->fontSmall == nullptr || impl_->fontBody == nullptr
        || impl_->fontMedium == nullptr || impl_->fontTitle == nullptr) {
        impl_->lastError = std::string("Opening font: ") + TTF_GetError();
        shutdown();
        return false;
    }

    impl_->initialized = true;
    return true;
}

void Gui::shutdown() {
    if (!impl_) {
        return;
    }
    if (impl_->fontTitle != nullptr) {
        TTF_CloseFont(impl_->fontTitle);
        impl_->fontTitle = nullptr;
    }
    if (impl_->fontMedium != nullptr) {
        TTF_CloseFont(impl_->fontMedium);
        impl_->fontMedium = nullptr;
    }
    if (impl_->fontBody != nullptr) {
        TTF_CloseFont(impl_->fontBody);
        impl_->fontBody = nullptr;
    }
    if (impl_->fontSmall != nullptr) {
        TTF_CloseFont(impl_->fontSmall);
        impl_->fontSmall = nullptr;
    }
    if (impl_->fontServiceInitialized) {
        plExit();
        impl_->fontServiceInitialized = false;
    }
    if (impl_->ttfInitialized) {
        TTF_Quit();
        impl_->ttfInitialized = false;
    }
    if (impl_->renderer != nullptr) {
        SDL_DestroyRenderer(impl_->renderer);
        impl_->renderer = nullptr;
    }
    if (impl_->window != nullptr) {
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    if (impl_->sdlInitialized) {
        SDL_Quit();
        impl_->sdlInitialized = false;
    }
    impl_->initialized = false;
}

bool Gui::ready() const {
    return impl_ != nullptr && impl_->initialized;
}

const std::string& Gui::error() const {
    return impl_->lastError;
}

void Gui::renderCatalog(const GuiCatalogView& view) {
    if (!ready()) {
        return;
    }
    impl_->beginFrame();
    impl_->header(view.version, view.device);

    const SDL_Rect sidebar{24, 108, 270, 526};
    const SDL_Rect content{318, 108, 938, 526};
    fillRect(impl_->renderer, sidebar, Panel);
    fillRect(impl_->renderer, content, Panel);
    outlineRect(impl_->renderer, sidebar, Border);
    outlineRect(impl_->renderer, content, Border);

    impl_->text(impl_->fontSmall, "PROFILE", 44, 130, TextMuted);
    impl_->text(impl_->fontMedium, view.profileName, 44, 156, Text, 230);
    impl_->text(impl_->fontSmall, view.profileLabel, 44, 190, TextMuted, 230);
    impl_->text(impl_->fontSmall, view.saveSummary, 44, 216, TextMuted, 230);

    impl_->text(impl_->fontSmall, "NEXTCLOUD", 44, 270, TextMuted);
    const SDL_Rect cloudBadge{44, 298, 226, 42};
    fillRect(
        impl_->renderer,
        cloudBadge,
        view.nextcloudConfigured ? AccentDark : SDL_Color{83, 48, 48, 255});
    impl_->text(
        impl_->fontBody,
        view.nextcloudConfigured
            ? (view.pendingCloudOperations == 0
                ? "Configured"
                : "Configured | Queue "
                    + std::to_string(view.pendingCloudOperations))
            : "Not configured",
        58,
        307,
        view.nextcloudConfigured ? Good : Danger,
        195);
    impl_->text(impl_->fontSmall, "STARTUP BACKUP", 44, 354, TextMuted);
    impl_->text(
        impl_->fontSmall,
        view.autoBackupOnStart ? "Enabled" : "Disabled",
        44,
        378,
        view.autoBackupOnStart ? Good : TextMuted,
        226);
    impl_->text(impl_->fontSmall, "RETENTION", 44, 414, TextMuted);
    impl_->text(
        impl_->fontSmall,
        view.retentionCount == 0
            ? "Disabled"
            : std::to_string(view.retentionCount) + " per game",
        44,
        438,
        view.retentionCount == 0 ? TextMuted : Good,
        226);
    impl_->text(impl_->fontSmall, "CONSOLE FOLDER", 44, 474, TextMuted);
    impl_->text(impl_->fontSmall, view.remoteRoot, 44, 498, Text, 226);

    if (!view.note.empty()) {
        impl_->text(impl_->fontSmall, "NOTE", 44, 542, Warning);
        impl_->text(impl_->fontSmall, view.note, 44, 566, TextMuted, 226);
    }

    impl_->text(impl_->fontMedium, "Saves", 340, 126, Text);
    impl_->text(impl_->fontSmall, view.pageLabel, 1120, 134, TextMuted, 110);
    if (!view.statusMessage.empty()) {
        impl_->text(
            impl_->fontSmall,
            view.statusMessage,
            340,
            160,
            toneColor(view.statusTone),
            870);
    } else {
        impl_->text(
            impl_->fontSmall,
            "Select a game and press A to create and upload its backup.",
            340,
            160,
            TextMuted,
            870);
    }
    if (!view.statusPath.empty()) {
        impl_->text(impl_->fontSmall, view.statusPath, 340, 184, TextMuted, 870);
    }

    if (!view.emptyMessage.empty()) {
        impl_->text(impl_->fontMedium, view.emptyMessage, 360, 300, TextMuted, 820);
    } else {
        const int firstRowY = 208;
        const int rowHeight = 44;
        for (std::size_t index = 0; index < view.rows.size(); ++index) {
            const GuiSaveRow& row = view.rows[index];
            const int y = firstRowY + static_cast<int>(index) * rowHeight;
            const SDL_Rect rowRect{336, y, 902, rowHeight - 4};
            fillRect(
                impl_->renderer,
                rowRect,
                row.selected ? PanelRaised : Panel);
            if (row.selected) {
                const SDL_Rect marker{336, y, 5, rowHeight - 4};
                fillRect(impl_->renderer, marker, Accent);
                outlineRect(impl_->renderer, rowRect, AccentDark);
            }
            impl_->text(
                impl_->fontBody,
                row.title,
                354,
                y + 7,
                row.selected ? Text : TextMuted,
                440);
            impl_->text(impl_->fontSmall, row.metadata, 820, y + 3, TextMuted, 400);
            const SDL_Rect statusMarker{820, y + 25, 10, 10};
            fillRect(impl_->renderer, statusMarker, saveStateColor(row.state));
            impl_->text(
                impl_->fontSmall,
                row.status,
                840,
                y + 19,
                saveStateColor(row.state),
                380);
        }
    }

    if (view.batchConfirmationPending) {
        const SDL_Rect overlay{318, 552, 938, 82};
        fillRect(impl_->renderer, overlay, SDL_Color{92, 67, 18, 250});
        outlineRect(impl_->renderer, overlay, Warning);
        impl_->text(
            impl_->fontBody,
            "Check all " + std::to_string(view.batchTotal)
                + " saves?",
            342,
            565,
            Text);
        impl_->text(
            impl_->fontSmall,
            "Press Y to start or B to cancel.",
            342,
            596,
            Warning);
    }

    const SDL_Rect footer{0, 650, ScreenWidth, 70};
    fillRect(impl_->renderer, footer, PanelRaised);
    impl_->buttonHint(20, 668, "A", "Backup", AccentDark);
    impl_->buttonHint(140, 668, "Y", "All", SDL_Color{129, 92, 20, 255});
    impl_->buttonHint(244, 668, "ZL", "Preflight", SDL_Color{35, 116, 110, 255});
    impl_->buttonHint(392, 668, "B", "Cloud", SDL_Color{147, 62, 62, 255});
    impl_->buttonHint(500, 668, "X", "Refresh", SDL_Color{43, 101, 155, 255});
    impl_->buttonHint(642, 668, "-", "Settings", SDL_Color{75, 85, 99, 255});
    impl_->buttonHint(792, 668, "L/R", "Profile", SDL_Color{75, 85, 99, 255});
    impl_->buttonHint(936, 668, "ZR", "Restore", SDL_Color{97, 55, 130, 255});
    impl_->buttonHint(1120, 668, "+", "Exit", SDL_Color{75, 85, 99, 255});
    impl_->present();
}

void Gui::renderProgress(const GuiProgressView& view) {
    if (!ready()) {
        return;
    }
    impl_->beginFrame();
    impl_->header(view.title, view.device);

    const SDL_Rect card{140, 128, 1000, 466};
    fillRect(impl_->renderer, card, Panel);
    outlineRect(impl_->renderer, card, Border);
    impl_->text(impl_->fontSmall, "OPERATION IN PROGRESS", 180, 164, Accent);
    impl_->text(impl_->fontTitle, view.stage, 180, 198, Text, 920);

    int detailY = 266;
    if (!view.profile.empty()) {
        impl_->text(impl_->fontSmall, "Profile", 180, detailY, TextMuted);
        impl_->text(impl_->fontBody, view.profile, 300, detailY - 3, Text, 760);
        detailY += 36;
    }
    if (!view.game.empty()) {
        impl_->text(impl_->fontSmall, "Game", 180, detailY, TextMuted);
        impl_->text(impl_->fontBody, view.game, 300, detailY - 3, Text, 760);
        detailY += 36;
    }
    if (!view.batch.empty()) {
        impl_->text(impl_->fontSmall, "Total", 180, detailY, TextMuted);
        impl_->text(impl_->fontBody, view.batch, 300, detailY - 3, Text, 760);
        detailY += 36;
    }
    if (!view.path.empty()) {
        impl_->text(impl_->fontSmall, view.path, 180, detailY + 8, TextMuted, 900);
    }

    if (view.batchTotal > 0) {
        const SDL_Rect batchTrack{180, 408, 920, 7};
        fillRect(impl_->renderer, batchTrack, SDL_Color{45, 57, 75, 255});
        const double batchRatio = std::min(
            1.0,
            static_cast<double>(view.batchCurrent)
                / static_cast<double>(view.batchTotal));
        const SDL_Rect batchBar{
            batchTrack.x,
            batchTrack.y,
            static_cast<int>(std::round(batchRatio * batchTrack.w)),
            batchTrack.h};
        fillRect(impl_->renderer, batchBar, Blue);
    }

    const SDL_Rect track{180, 446, 920, 18};
    fillRect(impl_->renderer, track, SDL_Color{45, 57, 75, 255});
    int progressWidth = 0;
    if (view.total > 0) {
        const double ratio = std::min(
            1.0,
            static_cast<double>(view.current) / static_cast<double>(view.total));
        progressWidth = static_cast<int>(std::round(ratio * track.w));
    }
    if (progressWidth > 0) {
        const SDL_Rect progressBar{track.x, track.y, progressWidth, track.h};
        fillRect(impl_->renderer, progressBar, Accent);
    }
    if (view.total > 0) {
        const unsigned percentage = static_cast<unsigned>(std::min<std::uint64_t>(
            100,
            (view.current * 100ULL) / view.total));
        impl_->text(
            impl_->fontSmall,
            std::to_string(percentage) + "%",
            1040,
            420,
            Text,
            60);
    }
    impl_->text(impl_->fontBody, view.amount, 180, 482, Text, 900);
    impl_->text(
        impl_->fontSmall,
        view.warning.empty()
            ? "Do not turn off the console or remove the microSD card."
            : view.warning,
        180,
        540,
        Warning,
        900);
    impl_->present();
}

void Gui::renderMenu(const GuiMenuView& view) {
    if (!ready()) {
        return;
    }
    impl_->beginFrame();
    impl_->header(view.version, view.device);

    const SDL_Rect content{72, 112, 1136, 512};
    fillRect(impl_->renderer, content, Panel);
    outlineRect(impl_->renderer, content, Border);
    impl_->text(impl_->fontTitle, view.title, 104, 130, Text, 900);
    impl_->text(impl_->fontSmall, view.breadcrumb, 104, 181, Accent, 1000);
    impl_->text(impl_->fontSmall, view.instruction, 104, 208, TextMuted, 1000);
    impl_->text(
        impl_->fontSmall,
        std::to_string(view.page + 1) + "/" + std::to_string(view.pageCount),
        1110,
        144,
        TextMuted,
        70);

    constexpr std::size_t RowsPerPage = 7;
    const std::size_t first = view.page * RowsPerPage;
    const std::size_t last = std::min(first + RowsPerPage, view.items.size());
    if (view.items.empty()) {
        impl_->text(impl_->fontMedium, "No items available", 120, 326, TextMuted);
    }
    for (std::size_t index = first; index < last; ++index) {
        const GuiMenuItem& item = view.items[index];
        const int y = 244 + static_cast<int>(index - first) * 50;
        const SDL_Rect row{96, y, 1088, 45};
        fillRect(impl_->renderer, row, index == view.selected ? PanelRaised : Panel);
        if (index == view.selected) {
            const SDL_Rect marker{96, y, 6, 45};
            fillRect(impl_->renderer, marker, Accent);
            outlineRect(impl_->renderer, row, AccentDark);
        }
        impl_->text(
            impl_->fontBody,
            item.primary,
            118,
            y + 4,
            index == view.selected ? Text : TextMuted,
            650);
        impl_->text(impl_->fontSmall, item.secondary, 118, y + 26, TextMuted, 720);
        impl_->text(
            impl_->fontSmall,
            item.badge,
            890,
            y + 12,
            toneColor(item.tone),
            270);
    }

    if (view.destructiveConfirmation) {
        const SDL_Rect overlay{132, 210, 1016, 330};
        fillRect(impl_->renderer, overlay, SDL_Color{75, 29, 34, 252});
        outlineRect(impl_->renderer, overlay, Danger);
        impl_->text(impl_->fontSmall, "DESTRUCTIVE RESTORE", 176, 248, Danger);
        impl_->text(impl_->fontTitle, view.confirmationTitle, 176, 284, Text, 920);
        impl_->text(impl_->fontBody, view.confirmationDetail, 176, 350, TextMuted, 920);
        impl_->text(
            impl_->fontBody,
            "Hold ZL + ZR and press A to confirm.",
            176,
            442,
            Warning,
            920);
        impl_->text(impl_->fontSmall, "B cancels", 176, 490, TextMuted);
    }

    const SDL_Rect footer{0, 650, ScreenWidth, 70};
    fillRect(impl_->renderer, footer, PanelRaised);
    impl_->buttonHint(32, 668, "A", "Select", AccentDark);
    impl_->buttonHint(210, 668, "B", "Back", SDL_Color{147, 62, 62, 255});
    impl_->buttonHint(392, 668, "↑↓", "Navigate", SDL_Color{75, 85, 99, 255});
    impl_->buttonHint(1010, 668, "+", "Exit", SDL_Color{75, 85, 99, 255});
    impl_->present();
}

void Gui::renderLogin(const GuiLoginView& view) {
    if (!ready()) return;
    impl_->beginFrame();
    impl_->header(view.version, view.device);
    fillRect(impl_->renderer, {48, 100, 1184, 524}, Panel);
    impl_->text(impl_->fontSmall, "NEXTCLOUD ACCOUNT", 80, 128, Accent);
    impl_->text(impl_->fontTitle, "Connect with your phone", 80, 160, Text, 680);
    impl_->text(impl_->fontBody, view.server, 80, 220, TextMuted, 660);
    impl_->text(impl_->fontMedium, "1. Scan the QR code", 80, 290, Text);
    impl_->text(impl_->fontBody, "Use your phone's camera to open Nextcloud.", 80, 328, TextMuted, 650);
    impl_->text(impl_->fontMedium, "2. Sign in and grant access", 80, 390, Text);
    impl_->text(impl_->fontBody, "NXSync will connect automatically after approval.", 80, 428, TextMuted, 650);
    impl_->text(impl_->fontSmall, view.status, 80, 510, view.canRetry ? Warning : Accent, 660);
    impl_->text(impl_->fontSmall, "You can revoke NXSync access in Nextcloud's Security settings.",
        80, 575, TextMuted, 1100);
    if (view.qr.size > 0 && view.qr.modules.size() == static_cast<std::size_t>(view.qr.size * view.qr.size)) {
        // Integer scaling and a four-module quiet zone preserve camera readability.
        const int scale = 390 / (view.qr.size + 8);
        const int pixels = (view.qr.size + 8) * scale;
        const int left = 792 + (390 - pixels) / 2;
        const int top = 158 + (390 - pixels) / 2;
        fillRect(impl_->renderer, {792, 158, 390, 390}, {255, 255, 255, 255});
        for (int y = 0; y < view.qr.size; ++y) {
            for (int x = 0; x < view.qr.size; ++x) {
                if (view.qr.modules[y * view.qr.size + x])
                    fillRect(impl_->renderer, {left + (x + 4) * scale, top + (y + 4) * scale, scale, scale}, {0, 0, 0, 255});
            }
        }
        const auto seconds = view.secondsRemaining % 60;
        impl_->text(impl_->fontSmall, "Expires in " + std::to_string(view.secondsRemaining / 60)
            + ":" + (seconds < 10 ? "0" : "") + std::to_string(seconds), 900, 552, TextMuted);
    } else {
        impl_->text(impl_->fontMedium, view.canRetry ? "Connection paused" : "Connecting...", 832, 320, TextMuted, 330);
    }
    fillRect(impl_->renderer, {0, 650, ScreenWidth, 70}, PanelRaised);
    if (view.canRetry) impl_->buttonHint(32, 668, "A", view.saveRetry ? "Retry save" : "New QR code", AccentDark);
    impl_->buttonHint(300, 668, "B", "Cancel", SDL_Color{147, 62, 62, 255});
    impl_->buttonHint(1010, 668, "+", "Exit", SDL_Color{75, 85, 99, 255});
    impl_->present();
}

void Gui::renderLoading(
    const std::string& title,
    const std::string& message,
    const std::string& detail) {
    if (!ready()) {
        return;
    }
    impl_->beginFrame();
    impl_->header(title, std::string());
    const SDL_Rect card{220, 210, 840, 260};
    fillRect(impl_->renderer, card, Panel);
    outlineRect(impl_->renderer, card, Border);
    const SDL_Rect marker{220, 210, 7, 260};
    fillRect(impl_->renderer, marker, Blue);
    impl_->text(impl_->fontTitle, message, 270, 268, Text, 740);
    impl_->text(impl_->fontBody, detail, 270, 338, TextMuted, 740);
    impl_->present();
}

} // namespace nxsync
