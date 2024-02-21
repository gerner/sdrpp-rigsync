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
    /* Name:            */ "sdrpp-rigctl-client",
    /* Description:     */ "Client for the RigCTL protocol",
    /* Author:          */ "Ryzerth;gener",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class BiDiRigctlClientModule : public ModuleManager::Instance {
public:
    BiDiRigctlClientModule(std::string name) {
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
        if (config.conf[name].contains("ifFreq")) {
            ifFreq = config.conf[name]["ifFreq"];
        }
        config.release();

        _retuneHandler.ctx = this;
        _retuneHandler.handler = retuneHandler;

        workerRunning = true;
        workerThread = std::thread(&BiDiRigctlClientModule::worker, this);

        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~BiDiRigctlClientModule() {
        stop();
        gui::menu.removeEntry(name);

        // let worker know we're shutting down
        std::unique_lock cv_lk(workerMutex);
        workerRunning = false;
        cv_lk.unlock();
        cv.notify_all();

        if (workerThread.joinable()) { workerThread.join(); }
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

        // Switch source to panadapter mode
        sigpath::sourceManager.setPanadpterIF(ifFreq);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::PANADAPTER);
        sigpath::sourceManager.onRetune.bindHandler(&_retuneHandler);

        running = true;
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        if (!running) { return; }

        // Switch source back to normal mode
        sigpath::sourceManager.onRetune.unbindHandler(&_retuneHandler);
        sigpath::sourceManager.setTuningMode(SourceManager::TuningMode::NORMAL);

        // Disconnect from rigctl server
        client->close();

        running = false;
    }

private:
    static void menuHandler(void* ctx) {
        BiDiRigctlClientModule* _this = (BiDiRigctlClientModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (_this->running) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##_rigctl_cli_host_", _this->name), _this->host, 1023)) {
            config.acquire();
            config.conf[_this->name]["host"] = std::string(_this->host);
            config.release(true);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##_rigctl_cli_port_", _this->name), &_this->port, 0, 0)) {
            config.acquire();
            config.conf[_this->name]["port"] = _this->port;
            config.release(true);
        }
        if (_this->running) { style::endDisabled(); }

        ImGui::LeftLabel("IF Frequency");
        ImGui::FillWidth();
        if (ImGui::InputDouble(CONCAT("##_rigctl_if_freq_", _this->name), &_this->ifFreq, 100.0, 100000.0, "%.0f")) {
            if (_this->running) {
                sigpath::sourceManager.setPanadpterIF(_this->ifFreq);
            }
            config.acquire();
            config.conf[_this->name]["ifFreq"] = _this->ifFreq;
            config.release(true);
        }

        ImGui::FillWidth();
        if (_this->running && ImGui::Button(CONCAT("Stop##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
            _this->stop();
        }
        else if (!_this->running && ImGui::Button(CONCAT("Start##_rigctl_cli_stop_", _this->name), ImVec2(menuWidth, 0))) {
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

    static void retuneHandler(double freq, void* ctx) {
        BiDiRigctlClientModule* _this = (BiDiRigctlClientModule*)ctx;
        std::lock_guard<std::recursive_mutex> lck(_this->mtx);
        _this->waterfallFrequency = freq;
        if (!_this->client || !_this->client->isOpen()) { return; }
        if (_this->waterfallFrequency == _this->rigFrequency) { return; }
        if (_this->client->setFreq(freq)) {
            flog::error("Could not set frequency");
        }
    }

    void worker() {
        // a comment in discord_integration claims SDR++ author is working on a
        // timer which we should probably be using instead of having a thread
        // wake up periodically
        std::unique_lock cv_lk(workerMutex);
        while (workerRunning) {
            {
                // synchronize state with the rig
                std::lock_guard<std::recursive_mutex> lck(mtx);
                if(running) {
                    double newFreq = client->getFreq();
                    if(newFreq < 0) {
                        flog::error("Could not get frequency from rig");
                    } else {
                        rigFrequency = newFreq;
                        if(rigFrequency != waterfallFrequency) {
                            //TODO: handle multiple VFOs?
                            //TODO: what's the right way to use ifFreq?
                            // we don't know what the radio's actual IF freq is
                            // so we're just approximating it
                            // this feels janky
                            if(abs(rigFrequency - ifFreq) > 500000) {
                                ifFreq = rigFrequency;
                                sigpath::sourceManager.setPanadpterIF(ifFreq);
                                config.acquire();
                                config.conf[name]["ifFreq"] = ifFreq;
                                config.release(true);
                            }
                            tuner::tune(tuner::TUNER_MODE_NORMAL, gui::waterfall.selectedVFO, rigFrequency);
                            // we will set waterfallFrequency to the new
                            // frequency when we get the retune callback
                        }
                    }
                }
            }
            cv.wait_for(cv_lk, std::chrono::milliseconds(250));
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    std::recursive_mutex mtx;

    char host[1024];
    int port = 4532;
    std::shared_ptr<net::rigctl::Client> client;

    double ifFreq = 8830000.0;

    EventHandler<double> _retuneHandler;

    double rigFrequency;
    double waterfallFrequency;

    // Threading
    std::thread workerThread;
    bool workerRunning;
    std::condition_variable cv;
    std::mutex workerMutex;
};

MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s() + "/rigctl_client_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new BiDiRigctlClientModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (BiDiRigctlClientModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
