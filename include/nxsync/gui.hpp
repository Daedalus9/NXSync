#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "nxsync/qr_code.hpp"

namespace nxsync {

enum class GuiSaveState {
    NotBackedUp,
    Changed,
    LocalCurrent,
    UploadPending,
    CloudCurrent,
    Error,
    Unknown,
};

enum class GuiTone {
    Accent,
    Neutral,
    Good,
    Warning,
    Danger,
};

struct GuiSaveRow {
    bool selected{false};
    std::string title;
    std::string metadata;
    std::string status;
    GuiSaveState state{GuiSaveState::Unknown};
};

struct GuiCatalogView {
    std::string version;
    std::string device;
    std::string remoteRoot;
    bool nextcloudConfigured{false};
    std::size_t pendingCloudOperations{0};
    bool autoBackupOnStart{false};
    std::size_t retentionCount{0};
    std::string profileLabel;
    std::string profileName;
    std::string saveSummary;
    std::string pageLabel;
    std::vector<GuiSaveRow> rows;
    std::string emptyMessage;
    std::string statusMessage;
    std::string statusPath;
    GuiTone statusTone{GuiTone::Accent};
    std::string note;
    bool batchConfirmationPending{false};
    std::size_t batchTotal{0};
};

struct GuiProgressView {
    std::string title;
    std::string device;
    std::string profile;
    std::string game;
    std::string batch;
    std::string stage;
    std::string path;
    std::string amount;
    std::string warning;
    std::uint64_t current{0};
    std::uint64_t total{0};
    std::size_t batchCurrent{0};
    std::size_t batchTotal{0};
};

struct GuiMenuItem {
    std::string primary;
    std::string secondary;
    std::string badge;
    GuiTone tone{GuiTone::Accent};
};

struct GuiMenuView {
    std::string version;
    std::string device;
    std::string title;
    std::string breadcrumb;
    std::string instruction;
    std::vector<GuiMenuItem> items;
    std::size_t selected{0};
    std::size_t page{0};
    std::size_t pageCount{1};
    bool destructiveConfirmation{false};
    std::string confirmationTitle;
    std::string confirmationDetail;
};

struct GuiLoginView {
    std::string version, device, server, status;
    QrCodeImage qr;
    unsigned secondsRemaining{0};
    bool canRetry{false};
    bool saveRetry{false};
};

class Gui {
public:
    Gui();
    ~Gui();

    Gui(const Gui&) = delete;
    Gui& operator=(const Gui&) = delete;

    bool initialize();
    void shutdown();
    bool ready() const;
    const std::string& error() const;

    void renderCatalog(const GuiCatalogView& view);
    void renderProgress(const GuiProgressView& view);
    void renderMenu(const GuiMenuView& view);
    void renderLogin(const GuiLoginView& view);
    void renderLoading(
        const std::string& title,
        const std::string& message,
        const std::string& detail = std::string());

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nxsync
