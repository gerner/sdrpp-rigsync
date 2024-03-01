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
#include <radio_interface.h>
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

std::map<int, net::rigctl::Mode> radioModeToRigctlMode = {
    { RADIO_IFACE_MODE_NFM, net::rigctl::MODE_FM },
    { RADIO_IFACE_MODE_WFM, net::rigctl::MODE_WFM },
    { RADIO_IFACE_MODE_AM, net::rigctl::MODE_AM },
    { RADIO_IFACE_MODE_DSB, net::rigctl::MODE_DSB },
    { RADIO_IFACE_MODE_USB, net::rigctl::MODE_USB },
    { RADIO_IFACE_MODE_CW, net::rigctl::MODE_CW },
    { RADIO_IFACE_MODE_LSB, net::rigctl::MODE_LSB },
};

net::rigctl::Mode getWaterfallMode(std::string vfoName) {
    int radioModuleMode;
    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_MODE, NULL, &radioModuleMode);
    auto it = radioModeToRigctlMode.find(radioModuleMode);
    if (radioModeToRigctlMode.end() == it) {
        return net::rigctl::MODE_INVALID;
    }
    return it->second;
}

bool setWaterfallMode(std::string vfoName, net::rigctl::Mode newMode) {
    auto it = std::find_if(radioModeToRigctlMode.begin(), radioModeToRigctlMode.end(), [&newMode](const auto& e) {
        return e.second == newMode;
    });
    if (it == radioModeToRigctlMode.end()) {
        return false;
    }
    int mode = it->first;
    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
    return true;
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
#ifdef GERNER_PROTO_RIGCTL
        if (config.conf[name].contains("syncMode")) {
            syncMode = config.conf[name]["syncMode"];
        }
#endif
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
        std::unique_lock cv_lk(workerMutex);
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
        workerErrors = 0;
        // initialize our view of where the waterfall is at
        double newFreq = gui::waterfall.getCenterFrequency();
        newFreq += sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
        waterfallFrequency = newFreq;
        rigFrequency = -2;

        //TODO: initialize mode/passband state

        // join old thread if it killed itself
        if (workerThread.joinable()) { workerThread.join(); }
        workerThread = std::thread(&RigSyncModule::worker, this);
        running = true;
    }

    void stop() {
        std::unique_lock cv_lk(workerMutex);
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        // let worker know we're shutting down
        cv_lk.unlock();
        cv.notify_all();
        running = false;
        if (workerThread.joinable()) { workerThread.join(); }
        // Disconnect from rigctl server
        client->close();
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

#ifdef GERNER_PROTO_RIGCTL
        {
            std::lock_guard<std::recursive_mutex> lck(_this->mtx);
            if (ImGui::Checkbox(CONCAT("Sync mode##_rigsync_sync_mode__", _this->name), &_this->syncMode)) {
                config.acquire();
                config.conf[_this->name]["syncMode"] = _this->syncMode;
                config.release();
            }
        }
#endif

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

#ifdef GERNER_PROTO_RIGCTL // SDR++ official doesn't implement rigctl get/setMode, but I have a fork that does
        //synchronize mode
        if (syncMode) {
            int newPassband;
            net::rigctl::Mode newMode = client->getMode(&newPassband);
            if (net::rigctl::MODE_INVALID == newMode) {
                // TODO: differentiate an error getting the mode vs the radio
                // having a mode our rigctl client doesn't understand
                flog::error("Could not get mode from radio");
                return false;
            } else {
                if (newMode != rigMode) {
                    rigMode = newMode;
                    if (rigMode != waterfallMode) {
                        flog::info("changing sdr++ radio mode from {0} to {1}", (int)waterfallMode, (int)rigMode);
                        if(setWaterfallMode(gui::waterfall.selectedVFO, rigMode)) {
                            waterfallMode = rigMode;
                        } else {
                            flog::error("error setting waterfall mode to {}", (int)rigMode);
                        }
                    }
                }
                if (newPassband != rigPassband) {
                    rigPassband = newPassband;
                    if (!almost_equal(rigPassband, waterfallPassband)) {
                        flog::info("changing sdr++ radio passband from {0} to {1}", waterfallPassband, rigPassband);
                        if (rigPassband > 0) {
                            core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &rigPassband, NULL);
                            waterfallPassband = rigPassband;
                        } else {
                            flog::error("got unexpected passband from rig {0}", rigPassband);
                        }
                    }
                }
            }
        }
#endif //GERNER_PROTO_RIGCTL

        return true;
    }

    bool syncRigWithWaterfall() {
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

#ifdef GERNER_PROTO_RIGCTL // SDR++ official doesn't implement rigctl get/setMode, but I have a fork that does
        if (syncMode) {
            // synchronize mode and passband
            net::rigctl::Mode newMode = getWaterfallMode(gui::waterfall.selectedVFO);
            if (newMode == net::rigctl::MODE_INVALID) {
                flog::error("waterfall had invalid mode");
                return false;
            } else {
                float radioPassband;
                core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &radioPassband);
                int newPassband = radioPassband;
                if (newMode != waterfallMode || newPassband != waterfallPassband) {
                    waterfallMode = newMode;
                    waterfallPassband = newPassband;
                    if (waterfallMode != rigMode || waterfallPassband != rigPassband) {
                        client->setMode(waterfallMode, waterfallPassband);
                    }
                }
            }
        }
#endif //GERNER_PROTO_RIGCTL
        return true;
    }

    void worker() {
        flog::info("rigsync worker starting");
        // this is a pure polling approach: we poll both the rig and the
        // waterfall for changes since the last time we checked
        std::unique_lock cv_lk(workerMutex);
        while (running) {
            {
                // synchronize state with the rig
                std::lock_guard<std::recursive_mutex> lck(mtx);
                if (!client || !client->isOpen()) {
                    flog::error("client is not open, stopping sync thread");
                    // client is already closed, we're about to stop, so just
                    // mark running as false to be stopped, user can restart us
                    running=false;
                    break;
                }
                if(running) {
                    bool success = true;
                    success = success && syncWaterfallWithRig();
                    success = success && syncRigWithWaterfall();
                    if (!success) {
                        workerErrors +=1;
                        if (workerErrors > maxWorkerErrors) {
                            flog::error("too many worker errors, stopping.");
                            client->close();
                            running = false;
                            break;
                        }
                    } else {
                        workerErrors = 0;
                    }
                }
            }
            cv.wait_for(cv_lk, std::chrono::milliseconds(pollPeriod));
        }
        flog::info("rigsync worker stopping");
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    int pollPeriod = 250;
#ifdef GERNER_PROTO_RIGCTL
    bool syncMode = true;
#endif
    std::shared_ptr<net::rigctl::Client> client;

    double rigFrequency;
    double waterfallFrequency;

    net::rigctl::Mode rigMode;
    net::rigctl::Mode waterfallMode;

    float rigPassband;
    float waterfallPassband;

    // Threading
    std::thread workerThread;
    std::condition_variable cv;
    std::mutex workerMutex;
    int workerErrors=0;
    int maxWorkerErrors=16;
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
