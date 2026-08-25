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
    | Unload() normally ran already; clean up defensively   |
    \*-----------------------------------------------------*/
    std::lock_guard<std::mutex> guard(mutex_);

    if(rm_ != nullptr)
    {
        for(HubEntry& entry : hubs_)
        {
            if(entry.controller != nullptr)
            {
                rm_->UnregisterRGBController(entry.controller);
                delete entry.controller;
                entry.controller = nullptr;
            }
        }

        rm_ = nullptr;
    }

    for(HubEntry& entry : hubs_)
    {
        delete entry.transport;
        entry.transport = nullptr;
    }

    hubs_.clear();
}

/*---------------------------------------------------------*\
| Plugin information                                        |
\*---------------------------------------------------------*/
OpenRGBPluginInfo UmbraPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name        = "AsiaHorse UMBRA";
    info.Description = "AsiaHorse UMBRA ARGB Hub support - 10 independent ARGB ports over USB HID";
    info.Version     = UMBRA_PLUGIN_VERSION;
    info.Commit      = "";
    info.URL         = "https://github.com/maihcx/AsiaHorse-Umbra-ARGB-Hub-SignalRGB-Plugin";
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
            new_entry.transport  = new UmbraController(device_info.path, device_info.serial);
            new_entry.controller = nullptr;
            hubs_.push_back(new_entry);
            entry = &hubs_.back();
        }

        if(entry->controller != nullptr)
        {
            continue;
        }

        /*-------------------------------------------------*\
        | Bring the transport up: handshake + topology +    |
        | software control handover                         |
        \*-------------------------------------------------*/
        if(!entry->transport->IsConnected() && !entry->transport->Initialize())
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
    | Only re-attempt when no hub has been exposed yet      |
    \*-----------------------------------------------------*/
    {
        std::lock_guard<std::mutex> guard(plugin->mutex_);

        bool any_exposed = false;

        for(const HubEntry& entry : plugin->hubs_)
        {
            if(entry.controller != nullptr)
            {
                any_exposed = true;
                break;
            }
        }

        if(any_exposed || plugin->rm_ == nullptr)
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
                            .arg(QString::fromStdString(entry.transport->GetLocation()));
                continue;
            }

            exposed++;

            html += QString("%1<br>").arg(QString::fromStdString(entry.controller->name));
            html += QString("&nbsp;&nbsp;%2<br>")
                        .arg(QString::fromStdString(entry.controller->location));

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
