/*---------------------------------------------------------*\
| UmbraWidget.h                                             |
|                                                           |
|   Status panel for the Umbra OpenRGB plugin               |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>

class UmbraWidget : public QWidget
{
    Q_OBJECT

public:
    explicit UmbraWidget(QWidget* parent = nullptr);

    /*-----------------------------------------------------*\
    | Renders rich-text status information built by the     |
    | plugin                                                |
    \*-----------------------------------------------------*/
    void SetStatusHtml(const QString& html);

signals:
    void RescanClicked();

private:
    QLabel*         title_label_;
    QLabel*         status_label_;
    QPushButton*    rescan_button_;
};
