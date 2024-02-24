#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <utils/proto/rigctl.h>
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <cctype>
#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdrpp-rigsync",
    /* Description:     */ "Synchronizes frequency across a rig and SDR++",
    /* Author:          */ "gener",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

bool almost_equal(double a, double b, double epsilon=1e-3) {
    return abs(a-b) < epsilon;
}

ConfigManager config;

class RigSyncModule : public ModuleManager::Instance {
public:
    RigSyncModule(std::string name) {
        this->name = name;

        // Load default
        strcpy(host, "127.0.0.1");

        // Load config
        config.acquire();
        if (config.conf[name].contains("host")) {
            std::string h = config.conf[name]["host"];
            strcpy(host, h.c_str());
        }
        if (config.conf[name].contains("port")) {
            port = config.conf[name]["port"];
            port = std::clamp<int>(port, 1, 65535);
        }
        config.release();

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~RigSyncModule() {
        stop();
        gui::menu.removeEntry(name);

    }

    void postInit() {
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (running) { return; }

        // Connect to rigctl server
        try {
            client = net::rigctl::connect(host, port);
        }
        catch (const std::exception& e) {
            flog::error("Could not connect: {}", e.what());
            return;
        }
        // clear known state, the worker thread will handle synchronization
        // initialize our view of where the waterfall is at
        double newFreq = gui::waterfall.getCenterFrequency();
        newFreq += sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
        waterfallFrequency = newFreq;
        rigFrequency = -2;

        workerRunning = true;
        workerThread = std::thread(&RigSyncModule::worker, this);

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        // let worker know we're shutting down
        {
            std::unique_lock cv_lk(workerMutex);
            workerRunning = false;
            cv_lk.unlock();
            cv.notify_all();
            if (workerThread.joinable()) { workerThread.join(); }
        }

        // Disconnect from rigctl server
        client->close();

        running = false;
    }

private:
    static void menuHandler(void* ctx) {
        RigSyncModule* _this = (RigSyncModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigsync_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigsync_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        ImGui::InputInt(CONCAT("Poll period##_rigsync_poll_period_", _this->name), &_this->pollPeriod, 0, 0);
        if (_this->running) { style::endDisabled(); }

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_rigsync_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_rigsync_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->start();
        }

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        {
            //protect access to the client by mtx
            std::lock_guard<std::recursive_mutex> lck(_this->mtx);
            if (_this->client && _this->client->isOpen() && _this->running) {
                ImGui::TextColored(ImVec4(0.0, 1.0, 0.0, 1.0), "Connected");
            }
            else if (_this->client && _this->running) {
                ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "Disconnected");
            }
            else {
                ImGui::TextUnformatted("Idle");
            }
        }
    }

    bool syncWaterfallWithRig() {
        //checks if we have a new rig frequency and sets the waterfall to that

        // synchronize rig -> waterfall
        double newFreq = client->getFreq();
        if(newFreq < 0) {
            flog::error("Could not get frequency from rig");
            return false;
        } else if(!almost_equal(rigFrequency, newFreq)) {
            rigFrequency = newFreq;
            if(!almost_equal(rigFrequency, waterfallFrequency)) {
                //TODO: handle multiple VFOs?
                flog::info("tuning waterfall from {0} to {1} with vfo {2}", waterfallFrequency, rigFrequency, gui::waterfall.selectedVFO);
                double oldWaterfallCenter = gui::waterfall.getCenterFrequency();
                tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, rigFrequency);
                // if we switch bands, sometimes the sdr doesn't get retuned
                // so let's do that so the sdr is always centered on the
                // waterfall
                if(gui::waterfall.getCenterFrequency() != oldWaterfallCenter) {
                    tuner::tune(tuner::TUNER_MODE_CENTER, gui::waterfall.selectedVFO, rigFrequency);
                }
                waterfallFrequency = rigFrequency;
            }
        } // else there's no change in rig freq, assume we're in sync
        if(waterfallFrequency != rigFrequency) {
            flog::error("waterfall and rig are out of sync!");
        }
        return true;
    }

    bool syncRigWithWaterfall() {
        //checks if we have a new waterfall frequency and sets the rig to that

        // from rigctl_server
        // Get center frequency of the SDR
        double newFreq = gui::waterfall.getCenterFrequency();
        // Add the offset of the VFO if it exists
        newFreq += sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);

        if(!almost_equal(waterfallFrequency, newFreq)) {
            waterfallFrequency = newFreq;
            if(!almost_equal(waterfallFrequency, rigFrequency)) {
                flog::info("tuning rig from {0} to {1}", rigFrequency, waterfallFrequency);
                if (client->setFreq(waterfallFrequency)) {
                    flog::error("Could not set rig frequency");
                    return false;
                }
                rigFrequency = waterfallFrequency;
            }
        } // else there's no change in waterfall freq, assume we're in sync
        if(waterfallFrequency != rigFrequency) {
            flog::error("waterfall and rig are out of sync!");
        }
        return true;
    }

    void worker() {
        // this is a pure polling approach: we poll both the rig and the
        // waterfall for changes since the last time we checked
        std::unique_lock cv_lk(workerMutex);
        while (workerRunning) {
            {
                // synchronize state with the rig
                std::lock_guard<std::recursive_mutex> lck(mtx);
                if (!client || !client->isOpen()) {
                    flog::error("client is not open, stopping sync thread");
                    break;
                }
                if(running) {
                    syncWaterfallWithRig();
                    syncRigWithWaterfall();
                }
            }
            cv.wait_for(cv_lk, std::chrono::milliseconds(pollPeriod));
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    int pollPeriod = 250;
    std::shared_ptr<net::rigctl::Client> client;

    double rigFrequency;
    double waterfallFrequency;

    // Threading
    std::thread workerThread;
    bool workerRunning;
    std::condition_variable cv;
    std::mutex workerMutex;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/sdrpp_rigsync_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RigSyncModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (RigSyncModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
