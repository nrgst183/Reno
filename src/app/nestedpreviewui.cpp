// Preview files of nested projects in the interface: see nestedpreviewui.h.
#include "nestedpreviewui.h"
#include "appstate.h"
#include "strconv.h"
#include "../engine/engine.h"
#include <QCoreApplication>
#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace reno {
namespace app {

namespace {

// One run of builds: the projects in the order they are built, shared by the thread building them and the window (or
// status bar) showing it.
struct PreviewRun {
    std::vector<std::string> paths;
    std::atomic<bool> cancel{false};
    std::mutex mutex;                       // guards what follows
    size_t index = 0;                       // the project being built
    int64_t done = 0, total = 0;            // its frames
    bool paused = false;                    // waiting while playback runs
    bool running = true;
    bool reported = false;                  // (interface thread) the end was shown
    int made = 0;
    std::vector<std::string> failed;        // "name: why"
    std::vector<std::string> changed;       // saved again while being built
};

std::shared_ptr<PreviewRun> gRun;           // the run going on, if any (interface thread)
// Builds cancelled by the user, with what they would have been made from: not asked for again until that changes.
std::map<std::string, uint64_t> gCancelled;

void runBuilds(std::shared_ptr<PreviewRun> run)
{
    lowerThreadPriority();
    for (size_t k = 0; k < run->paths.size() && !run->cancel.load(); ++k) {
        const std::string& path = run->paths[k];
        {
            std::lock_guard<std::mutex> lk(run->mutex);
            run->index = k;
            run->done = 0;
            run->total = 0;
        }
        std::string message;
        const int r = buildNestedPreview(path, [run](int64_t done, int64_t total, bool paused) {
            std::lock_guard<std::mutex> lk(run->mutex);
            run->done = done;
            run->total = total;
            run->paused = paused;
            return !run->cancel.load();
        }, &message);
        std::lock_guard<std::mutex> lk(run->mutex);
        if (r == 0) ++run->made;
        else if (r == 1) run->failed.push_back(pathFileName(path) + ": " + message);
        else if (r == 3) run->changed.push_back(pathFileName(path));
        else break;
    }
    std::lock_guard<std::mutex> lk(run->mutex);
    run->running = false;
}

// What a run did, for the status bar; the cancelled ones are remembered.
std::string wrapUp(PreviewRun& run)
{
    if (run.reported) return std::string();
    run.reported = true;
    if (gRun.get() == &run) gRun.reset();
    const bool cancelled = run.cancel.load();
    if (cancelled)
        for (size_t k = run.index; k < run.paths.size(); ++k) gCancelled[run.paths[k]] = nestedPreviewSignature(run.paths[k]);
    std::string s = cancelled ? strfmt("Nested project previews stopped: %d of %d made", run.made, (int)run.paths.size())
                              : strfmt("Nested project previews made: %d", run.made);
    if (!run.failed.empty()) s += strfmt("; %d could not be made (%s)", (int)run.failed.size(), run.failed.front().c_str());
    if (!run.changed.empty()) s += strfmt("; %s changed while being rendered and will be made again", run.changed.front().c_str());
    return s + ".";
}

// The progress, on the status bar (Run in Background, or where no window can be shown).
void watchInBackground(std::shared_ptr<PreviewRun> run)
{
    auto* timer = new QTimer(QCoreApplication::instance());
    timer->setObjectName("nestedPreviewWatch");
    timer->setInterval(250);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, run] {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(run->mutex);
            if (run->running) {
                if (run->paused) line = "Nested project previews wait while playing";
                else if (run->total > 0)
                    line = strfmt("Building nested project previews in the background: %s (%d of %d), %lld of %lld frames",
                                  pathFileName(run->paths[run->index]).c_str(), (int)run->index + 1, (int)run->paths.size(),
                                  (long long)run->done, (long long)run->total);
            }
        }
        if (!line.empty()) {
            AppState::get().status(line);
            return;
        }
        bool running;
        {
            std::lock_guard<std::mutex> lk(run->mutex);
            running = run->running;
        }
        if (running) return;
        timer->stop();
        timer->deleteLater();
        const std::string s = wrapUp(*run);
        if (!s.empty()) AppState::get().status(s);
    });
    timer->start();
}

// The window, as VEGAS shows one while it renders the nested projects it has to: which project, how far, and Cancel.
class PreviewDialog : public QDialog {
public:
    PreviewDialog(QWidget* parent, std::shared_ptr<PreviewRun> run) : QDialog(parent), run_(std::move(run))
    {
        setObjectName("nestedPreviewDialog");
        setWindowTitle("Nested Project Previews");
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumWidth(460);
        auto* root = new QVBoxLayout(this);
        title_ = new QLabel("Building nested project previews...", this);
        title_->setObjectName("nestedPreviewTitle");
        QFont f = title_->font();
        f.setBold(true);
        title_->setFont(f);
        what_ = new QLabel("Starting...", this);
        what_->setObjectName("nestedPreviewWhat");
        what_->setTextFormat(Qt::PlainText);
        bar_ = new QProgressBar(this);
        bar_->setObjectName("nestedPreviewBar");
        bar_->setRange(0, 0);
        count_ = new QLabel(this);
        count_->setObjectName("nestedPreviewCount");
        summary_ = new QLabel(this);
        summary_->setObjectName("nestedPreviewSummary");
        summary_->setWordWrap(true);
        summary_->setText("Until it is done, nested projects that have no preview yet are rendered as they play.");
        root->addWidget(title_);
        root->addWidget(what_);
        root->addWidget(bar_);
        root->addWidget(count_);
        root->addWidget(summary_);
        auto* row = new QHBoxLayout();
        background_ = new QPushButton("Run in &Background", this);
        background_->setObjectName("nestedPreviewBackground");
        background_->setToolTip("Close this window and go on working: the status bar shows how far it is");
        row->addWidget(background_);
        row->addStretch(1);
        cancel_ = new QPushButton("Cancel", this);
        cancel_->setObjectName("nestedPreviewCancel");
        cancel_->setToolTip("Stop: the nested projects not done yet are rendered as they play (Tools > Rebuild Nested Project Previews makes them later)");
        close_ = new QPushButton("Close", this);
        close_->setObjectName("nestedPreviewClose");
        close_->hide();
        row->addWidget(cancel_);
        row->addWidget(close_);
        root->addLayout(row);

        QObject::connect(cancel_, &QPushButton::clicked, [this] { stop(); });
        QObject::connect(close_, &QPushButton::clicked, [this] { accept(); });
        QObject::connect(background_, &QPushButton::clicked, [this] {
            inBackground_ = true;
            watchInBackground(run_);
            accept();
        });
        poll_ = new QTimer(this);
        poll_->setInterval(40);
        QObject::connect(poll_, &QTimer::timeout, [this] { update(); });
        poll_->start();
    }

    // closed early (Esc, the window's X): stops like Cancel, unless sent to the background
    void reject() override
    {
        if (!finished_ && !inBackground_) {
            stop();
            return;                         // closed once the frame being rendered is done (update)
        }
        QDialog::reject();
    }

private:
    void stop()
    {
        run_->cancel = true;
        cancel_->setEnabled(false);
        background_->setEnabled(false);
        what_->setText("Stopping...");
    }

    void update()
    {
        if (finished_ || inBackground_) return;
        bool running, paused;
        size_t index;
        int64_t done, total;
        {
            std::lock_guard<std::mutex> lk(run_->mutex);
            running = run_->running;
            paused = run_->paused;
            index = run_->index;
            done = run_->done;
            total = run_->total;
        }
        if (!running) {
            finish();
            return;
        }
        if (run_->cancel.load()) return;
        const std::string name = pathFileName(run_->paths[index]);
        what_->setText(paused ? QString("%1 (%2 of %3) - waiting while playing").arg(qs(name)).arg(index + 1).arg(run_->paths.size())
                              : QString("%1 (%2 of %3)").arg(qs(name)).arg(index + 1).arg(run_->paths.size()));
        if (total > 0) {
            bar_->setRange(0, (int)total);
            bar_->setValue((int)done);
            count_->setText(QString("%1 of %2 frames").arg(done).arg(total));
        }
    }

    void finish()
    {
        finished_ = true;
        poll_->stop();
        const bool cancelled = run_->cancel.load();
        const bool trouble = !run_->failed.empty() || !run_->changed.empty();
        const std::string status = wrapUp(*run_);
        if (!status.empty()) AppState::get().status(status);
        // all made: the window goes by itself
        if (!cancelled && !trouble) {
            accept();
            return;
        }
        title_->setText(cancelled ? "Stopped" : "Finished");
        what_->setText("");
        if (bar_->maximum() == 0) bar_->setRange(0, 1);
        count_->setText("");
        QString text = cancelled ? QString("Stopped. The nested projects not done are rendered as they play.\n")
                                 : QString("%1 of %2 made.\n").arg(run_->made).arg(run_->paths.size());
        for (const std::string& f : run_->failed) text += QString("Not made: %1\n").arg(qs(f));
        for (const std::string& c : run_->changed) text += QString("%1 was saved again while it was being rendered; it is made again next time.\n").arg(qs(c));
        summary_->setText(text.trimmed());
        cancel_->hide();
        background_->hide();
        close_->show();
        close_->setDefault(true);
    }

    std::shared_ptr<PreviewRun> run_;
    QLabel *title_ = nullptr, *what_ = nullptr, *count_ = nullptr, *summary_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QPushButton *cancel_ = nullptr, *close_ = nullptr, *background_ = nullptr;
    QTimer* poll_ = nullptr;
    bool finished_ = false, inBackground_ = false;
};

}  // namespace

void checkNestedPreviews(QWidget* owner, bool all)
{
    AppState& app = AppState::get();
    if (!all && !app.prefs.buildNestedPreviews) return;
    if (gRun) {
        if (all) app.status("Nested project previews are being made already.");
        return;
    }
    std::vector<std::string> list = nestedPreviewsNeeded(app.project(), all);
    if (all) {
        for (const std::string& p : list) gCancelled.erase(p);
    } else {
        list.erase(std::remove_if(list.begin(), list.end(), [](const std::string& p) {
                       auto it = gCancelled.find(p);
                       return it != gCancelled.end() && it->second == nestedPreviewSignature(p);
                   }),
                   list.end());
    }
    if (list.empty()) {
        if (all) app.status("The project uses no nested project that shows pictures.");
        return;
    }
    gRun = std::make_shared<PreviewRun>();
    gRun->paths = list;
    std::thread(runBuilds, gRun).detach();
    if (!owner || QGuiApplication::platformName() == "offscreen") {
        watchInBackground(gRun);
        return;
    }
    auto* dlg = new PreviewDialog(owner, gRun);
    dlg->show();
}

void stopNestedPreviews()
{
    std::shared_ptr<PreviewRun> run = gRun;
    if (!run) return;
    run->cancel = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(run->mutex);
            if (!run->running) break;
        }
        if (std::chrono::steady_clock::now() > until) break;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    run->reported = true;                   // (Reno is closing: nothing is shown or remembered)
    gRun.reset();
}

}  // namespace app
}  // namespace reno
