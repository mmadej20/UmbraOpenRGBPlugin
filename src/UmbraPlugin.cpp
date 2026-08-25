/*---------------------------------------------------------*\
| UmbraPlugin.cpp                                           |
|                                                           |
|   OpenRGB Plugin API v4 entry point for the AsiaHorse     |
|   UMBRA ARGB Hub plugin                                   |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "UmbraPlugin.h"

#include "RGBController_Umbra.h"

#include <cstdio>

UmbraPlugin::UmbraPlugin()
    : rm_(nullptr), widget_(nullptr)
{
}

UmbraPlugin::~UmbraPlugin()
{
    /*-----------------------------------------------------*\
    | Unload() normally ran already; clean up defensively.  |
    | The detection-end callback must be removed too -      |
    | otherwise ResourceManager would keep a dangling this  |
    | pointer when this instance is destroyed               |
    \*-----------------------------------------------------*/
    std::lock_guard<std::mutex> guard(mutex_);

    if(rm_ != nullptr)
    {
        rm_->UnregisterDetectionEndCallback(&UmbraPlugin::DetectionEndCallback, this);

        for(HubEntry& entry : hubs_)
        {
            if(entry.controller != nullptr)
            {
                rm_->UnregisterRGBController(entry.controller);
                delete entry.controller;
                entry.controller = nullptr;
            }
        }

        for(HubEntry& entry : hubs_)
        {
            delete entry.transport;
            entry.transport = nullptr;
        }

        hubs_.clear();

        rm_ = nullptr;
    }
}

/*---------------------------------------------------------*\
| Plugin information                                        |
\*---------------------------------------------------------*/
OpenRGBPluginInfo UmbraPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name        = "AsiaHorse UMBRA";
    info.Description = "AsiaHorse UMBRA ARGB Hub support - 10 independent ARGB ports over USB HID. "
                       "Protocol based on the SignalRGB plugin by maihcx.";
    info.Version     = UMBRA_PLUGIN_VERSION;
    info.Commit      = "";
    info.URL         = "https://github.com/mmadej20/UmbraOpenRGBPlugin";
    info.Icon        = QImage();

    info.Location     = OPENRGB_PLUGIN_LOCATION_DEVICES;
    info.Label        = "UMBRA";
    info.TabIconString = "";
    info.TabIcon      = QImage();

    return info;
}

unsigned int UmbraPlugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;
}

/*---------------------------------------------------------*\
| Plugin lifecycle                                          |
\*---------------------------------------------------------*/
void UmbraPlugin::Load(ResourceManagerInterface* resource_manager_ptr)
{
    rm_ = resource_manager_ptr;

    if(rm_ == nullptr)
    {
        return;
    }

    UmbraController::InitHidapi();

    DetectControllers();

    /*-----------------------------------------------------*\
    | If detection above failed (e.g. the hub was busy      |
    | during early startup), retry after each ResourceManager|
    | detection run                                         |
    \*-----------------------------------------------------*/
    rm_->RegisterDetectionEndCallback(&UmbraPlugin::DetectionEndCallback, this);
}

QWidget* UmbraPlugin::GetWidget()
{
    /*-----------------------------------------------------*\
    | Called from the GUI thread; rc3 embeds this widget    |
    | directly into a tab, so it must never be null         |
    \*-----------------------------------------------------*/
    if(widget_ == nullptr)
    {
        widget_ = new UmbraWidget();
        connect(widget_, &UmbraWidget::RescanClicked, this, &UmbraPlugin::Rescan);
    }

    UpdateWidgetStatus();

    return widget_;
}

QMenu* UmbraPlugin::GetTrayMenu()
{
    return nullptr;
}

void UmbraPlugin::Unload()
{
    std::lock_guard<std::mutex> guard(mutex_);

    if(rm_ != nullptr)
    {
        rm_->UnregisterDetectionEndCallback(&UmbraPlugin::DetectionEndCallback, this);
    }

    RemoveControllers();

    rm_ = nullptr;

    /*-----------------------------------------------------*\
    | The widget is parented into the OpenRGB UI and is     |
    | destroyed together with the tab; just drop the pointer|
    \*-----------------------------------------------------*/
    widget_ = nullptr;
}

/*---------------------------------------------------------*\
| Detection                                                 |
\*---------------------------------------------------------*/
void UmbraPlugin::DetectControllers()
{
    std::vector<UmbraController::DeviceInfo> found = UmbraController::EnumerateDevices();

    std::lock_guard<std::mutex> guard(mutex_);

    if(rm_ == nullptr)
    {
        return;
    }

    auto path_listed = [&found](const std::string& path)
    {
        for(const UmbraController::DeviceInfo& info : found)
        {
            if(info.path == path)
            {
                return true;
            }
        }

        return false;
    };

    /*-----------------------------------------------------*\
    | Reaping pass                                          |
    |                                                       |
    | Two independent death conditions:                     |
    |                                                       |
    | 1. The local HID handle broke (write failure, driver  |
    |    reset). IsConnected() reports false while the      |
    |    device may still be enumerated - the exposed       |
    |    controller is dropped so the attach pass below     |
    |    performs a clean reconnect.                        |
    |                                                       |
    | 2. The device disappeared from enumeration. This      |
    |    catches the unplug-with-static-RGB case where no   |
    |    further hid_write() ever fails and the stale       |
    |    handle still looks "connected". To avoid killing   |
    |    a hub on a single transient enumeration miss, the  |
    |    entry is only released after several consecutive   |
    |    misses.                                            |
    \*-----------------------------------------------------*/
    for(auto it = hubs_.begin(); it != hubs_.end(); )
    {
        HubEntry& entry     = *it;
        const bool listed   = path_listed(entry.transport->GetPath());
        const bool connected= entry.transport->IsConnected();

        entry.missed_scans = listed ? 0u : entry.missed_scans + 1u;

        /* Case 1: broken handle */
        if(entry.controller != nullptr && !connected)
        {
            rm_->UnregisterRGBController(entry.controller);
            delete entry.controller;
            entry.controller = nullptr;
        }

        /* Case 2: absent from the bus for good */
        if(!listed && entry.missed_scans >= MISSED_SCANS_BEFORE_DROP)
        {
            if(entry.controller != nullptr)
            {
                rm_->UnregisterRGBController(entry.controller);
                delete entry.controller;
                entry.controller = nullptr;
            }

            delete entry.transport;
            it = hubs_.erase(it);
            continue;
        }

        ++it;
    }

    for(const UmbraController::DeviceInfo& device_info : found)
    {
        /*-------------------------------------------------*\
        | Find or create this hub's transport               |
        \*-------------------------------------------------*/
        HubEntry* entry = nullptr;

        for(HubEntry& candidate : hubs_)
        {
            if(candidate.transport->GetPath() == device_info.path)
            {
                entry = &candidate;
                break;
            }
        }

        if(entry == nullptr)
        {
            HubEntry new_entry;
            new_entry.transport    = new UmbraController(device_info.path, device_info.serial);
            new_entry.controller   = nullptr;
            new_entry.missed_scans = 0;
            hubs_.push_back(new_entry);
            entry = &hubs_.back();
        }

        if(entry->controller != nullptr)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | Bring the transport up.                           |
        |                                                   |
        | An already-connected transport means an earlier   |
        | init succeeded but reported zero populated ports; |
        | re-read the topology instead of skipping forever  |
        | so devices connected later get picked up.         |
        \*-------------------------------------------------*/
        bool ready = false;

        if(entry->transport->IsConnected())
        {
            ready = entry->transport->RefreshTopology();
        }
        else
        {
            ready = entry->transport->Initialize();
        }

        if(!ready)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | Do not expose a hub with no populated ports -     |
        | an RGBController without LEDs breaks the UI       |
        \*-------------------------------------------------*/
        if(entry->transport->GetTotalLedCount() == 0)
        {
            continue;
        }

        entry->controller = new RGBController_Umbra(entry->transport);

        rm_->RegisterRGBController(entry->controller);
    }
}

void UmbraPlugin::RemoveControllers()
{
    /*-----------------------------------------------------*\
    | mutex_ must be held by the caller                     |
    \*-----------------------------------------------------*/

    /*-----------------------------------------------------*\
    | First pass: unregister controllers. Deleting each one |
    | joins its update thread, so afterwards nothing can    |
    | reference the transports anymore                      |
    \*-----------------------------------------------------*/
    for(HubEntry& entry : hubs_)
    {
        if(entry.controller != nullptr)
        {
            if(rm_ != nullptr)
            {
                rm_->UnregisterRGBController(entry.controller);
            }

            delete entry.controller;
            entry.controller = nullptr;
        }
    }

    /*-----------------------------------------------------*\
    | Second pass: release transports                       |
    \*-----------------------------------------------------*/
    for(HubEntry& entry : hubs_)
    {
        delete entry.transport;
        entry.transport = nullptr;
    }

    hubs_.clear();
}

void UmbraPlugin::DetectionEndCallback(void* this_ptr)
{
    UmbraPlugin* plugin = (UmbraPlugin*)this_ptr;

    if(plugin == nullptr)
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Re-attempt detection after every ResourceManager run. |
    | DetectControllers() deduplicates by path and skips    |
    | hubs that are already exposed, so this is safe for:   |
    |  - a hub that was busy during startup,                |
    |  - hot-plugged additional hubs,                       |
    |  - hubs whose ports were populated later              |
    \*-----------------------------------------------------*/
    {
        std::lock_guard<std::mutex> guard(plugin->mutex_);

        if(plugin->rm_ == nullptr)
        {
            return;
        }
    }

    plugin->DetectControllers();
}

void UmbraPlugin::Rescan()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        RemoveControllers();
    }

    DetectControllers();

    UpdateWidgetStatus();
}

/*---------------------------------------------------------*\
| Widget status                                             |
\*---------------------------------------------------------*/
void UmbraPlugin::UpdateWidgetStatus()
{
    /* GUI thread only */
    if(widget_ == nullptr)
    {
        return;
    }

    QString html;
    int exposed = 0;

    {
        std::lock_guard<std::mutex> guard(mutex_);

        html += QString("<b>Detected hubs:</b> %1<br>").arg((int)hubs_.size());

        for(const HubEntry& entry : hubs_)
        {
            html += "<hr>";

            if(entry.controller == nullptr)
            {
                html += QString("%1<br>&nbsp;&nbsp;Not initialized (busy or no devices connected)")
                            .arg(QString::fromStdString(entry.transport->GetLocation()).toHtmlEscaped());
                continue;
            }

            exposed++;

            html += QString("%1<br>")
                        .arg(QString::fromStdString(entry.controller->name).toHtmlEscaped());
            html += QString("&nbsp;&nbsp;%2<br>")
                        .arg(QString::fromStdString(entry.controller->location).toHtmlEscaped());

            std::string ports_summary;

            for(unsigned int port = 0; port < UmbraController::NUM_PORTS; port++)
            {
                char part[16];
                snprintf(part, sizeof(part), "P%02u:%u ", port + 1u,
                         entry.transport->GetPortLedCount(port));
                ports_summary += part;
            }

            html += QString("&nbsp;&nbsp;Ports: %1")
                        .arg(QString::fromStdString(ports_summary));
        }
    }

    if(exposed > 0)
    {
        html += QString("<hr>Tip: each populated port appears as an independent zone in Direct mode.");
    }

    widget_->SetStatusHtml(html);
}
