/*---------------------------------------------------------*\
| UmbraWidget.cpp                                           |
|                                                           |
|   Status panel for the Umbra OpenRGB plugin               |
|                                                           |
|   This file is part of the UmbraOpenRGBPlugin project.    |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "UmbraWidget.h"

#include <QVBoxLayout>

UmbraWidget::UmbraWidget(QWidget* parent)
    : QWidget(parent),
      title_label_(nullptr),
      status_label_(nullptr),
      rescan_button_(nullptr)
{
    QVBoxLayout* layout = new QVBoxLayout(this);

    title_label_ = new QLabel("<b>AsiaHorse UMBRA ARGB Hub</b>", this);
    status_label_ = new QLabel("Initializing...", this);
    rescan_button_ = new QPushButton("Re-scan devices", this);

    status_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    status_label_->setTextFormat(Qt::RichText);
    status_label_->setWordWrap(true);
    status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_label_->setMinimumHeight(120);

    layout->addWidget(title_label_);
    layout->addWidget(status_label_);
    layout->addStretch(1);
    layout->addWidget(rescan_button_, 0, Qt::AlignLeft);

    connect(rescan_button_, &QPushButton::clicked, this, [this]()
    {
        emit RescanClicked();
    });
}

void UmbraWidget::SetStatusHtml(const QString& html)
{
    if(status_label_ != nullptr)
    {
        status_label_->setText(html);
    }
}
