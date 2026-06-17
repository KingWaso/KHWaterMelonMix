/*
    Copyright 2016-2026 melonDS team / KHWaterMelonMix

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#include "RelayDialog.h"
#include "Config.h"
#include "main.h"
#include "Relay.h"
#include "MPInterface.h"

#include <QMessageBox>
#include <QStandardItemModel>
#include <QClipboard>
#include <QApplication>

#include "ui_RelayHostDialog.h"
#include "ui_RelayClientDialog.h"
#include "ui_RelaySessionDialog.h"

using namespace melonDS;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Relay& relay() { return (Relay&)MPInterface::Get(); }

RelaySessionDialog* relaySessionDlg = nullptr;

// ── RelayHostDialog ───────────────────────────────────────────────────────────

RelayHostDialog::RelayHostDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::RelayHostDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    // Pre-fill player name from config
    auto cfg = Config::GetGlobalTable();
    ui->txtPlayerName->setText(cfg.GetQString("Relay.PlayerName"));
    ui->sbNumPlayers->setRange(2, 16);
    ui->sbNumPlayers->setValue(cfg.GetInt("Relay.HostNumPlayers"));

    // Show the machine's outbound IP so the host knows what to share
    QString localIP = QString::fromStdString(GetLocalIPAddress());
    ui->lblYourIP->setText(QString("Your IP address: <b>%1</b>  "
        "(share this + the room code with friends)").arg(localIP));
}

RelayHostDialog::~RelayHostDialog()
{
    delete ui;
}

void RelayHostDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        return;
    }

    if (r == QDialog::Accepted)
    {
        QString name = ui->txtPlayerName->text().trimmed();
        if (name.isEmpty())
        {
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Please enter a player name.");
            return;
        }

        int numPlayers = ui->sbNumPlayers->value();

        setMPInterface(MPInterface_Relay);

        if (!relay().HostGame(name.toStdString().c_str(), numPlayers))
        {
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Failed to start the relay server.\n"
                                 "Port 7100 may already be in use.");
            setMPInterface(MPInterface_Local);
            return;
        }

        // Save prefs
        auto cfg = Config::GetGlobalTable();
        cfg.SetString("Relay.PlayerName", name.toStdString());
        cfg.SetInt("Relay.HostNumPlayers", numPlayers);
        Config::Save();

        // Open the session dialog (shows room code + player list)
        relaySessionDlg = RelaySessionDialog::openDlg(parentWidget());
    }

    QDialog::done(r);
}

// ── RelayClientDialog ─────────────────────────────────────────────────────────

RelayClientDialog::RelayClientDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::RelayClientDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    auto cfg = Config::GetGlobalTable();
    ui->txtPlayerName->setText(cfg.GetQString("Relay.PlayerName"));
    ui->txtHostIP->setText(cfg.GetQString("Relay.LastHostIP"));

    // Show local IP for reference (useful if player is also the NAT router)
    QString localIP = QString::fromStdString(GetLocalIPAddress());
    ui->lblYourIP->setText(QString("Your IP address: <b>%1</b>").arg(localIP));
}

RelayClientDialog::~RelayClientDialog()
{
    delete ui;
}

void RelayClientDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        return;
    }

    if (r == QDialog::Accepted)
    {
        QString name   = ui->txtPlayerName->text().trimmed();
        QString hostIP = ui->txtHostIP->text().trimmed();
        QString code   = ui->txtRoomCode->text().trimmed();

        if (name.isEmpty())
        {
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Please enter a player name.");
            return;
        }
        if (hostIP.isEmpty())
        {
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Please enter the host's IP address.");
            return;
        }
        if (code.length() != 6)
        {
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Room code must be exactly 6 digits.");
            return;
        }

        setMPInterface(MPInterface_Relay);
        setEnabled(false);

        if (!relay().JoinGame(name.toStdString().c_str(),
                              hostIP.toStdString().c_str(),
                              code.toStdString().c_str()))
        {
            setEnabled(true);
            setMPInterface(MPInterface_Local);
            QMessageBox::warning(this, "KHWaterMelonMix",
                                 QString("Could not connect to %1.\n"
                                         "Check the IP address and room code.").arg(hostIP));
            return;
        }

        setEnabled(true);

        auto cfg = Config::GetGlobalTable();
        cfg.SetString("Relay.PlayerName",  name.toStdString());
        cfg.SetString("Relay.LastHostIP",  hostIP.toStdString());
        Config::Save();

        relaySessionDlg = RelaySessionDialog::openDlg(parentWidget());
    }

    QDialog::done(r);
}

// ── RelaySessionDialog ────────────────────────────────────────────────────────

RelaySessionDialog::RelaySessionDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::RelaySessionDialog)
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose);

    // Player list table
    QStandardItemModel* model = new QStandardItemModel();
    ui->tvPlayerList->setModel(model);
    model->setHorizontalHeaderLabels({"#", "Player", "Status", "Ping"});

    // Room code / IP info
    if (MPInterface::GetType() == MPInterface_Relay)
    {
        QString code = QString::fromLatin1(relay().GetRoomCode());
        QString ip   = QString::fromStdString(GetLocalIPAddress());

        if (relay().IsHosting())
        {
            ui->lblRoomInfo->setText(
                QString("Room code: <b>%1</b>   |   Your IP: <b>%2</b><br>"
                        "Share both with friends so they can join.")
                .arg(code, ip));

            // Copy-code button only shown for host
            ui->btnCopyCode->setVisible(true);
            connect(ui->btnCopyCode, &QPushButton::clicked, this, [code](){
                QApplication::clipboard()->setText(code);
            });
        }
        else
        {
            ui->lblRoomInfo->setText(
                QString("Connected to room <b>%1</b>").arg(code));
            ui->btnCopyCode->setVisible(false);
        }
    }

    refreshTimer = new QTimer(this);
    connect(refreshTimer, &QTimer::timeout, this, &RelaySessionDialog::doUpdatePlayerList);
    refreshTimer->start(1000);

    doUpdatePlayerList();
}

RelaySessionDialog::~RelaySessionDialog()
{
    delete ui;
}

void RelaySessionDialog::on_btnLeave_clicked()
{
    done(QDialog::Accepted);
}

void RelaySessionDialog::done(int r)
{
    if (!((MainWindow*)parent())->getEmuInstance())
    {
        QDialog::done(r);
        return;
    }

    bool warn = (MPInterface::GetType() == MPInterface_Relay &&
                 relay().GetNumPlayers() >= 2);

    if (warn)
    {
        if (QMessageBox::warning(this, "KHWaterMelonMix",
                                 "Really leave this game?",
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::No)
            return;
    }

    relay().EndSession();
    setMPInterface(MPInterface_Local);
    relaySessionDlg = nullptr;

    QDialog::done(r);
}

void RelaySessionDialog::doUpdatePlayerList()
{
    if (MPInterface::GetType() != MPInterface_Relay) return;

    auto players   = relay().GetPlayerList();
    int  maxplayers = relay().GetMaxPlayers();

    QStandardItemModel* model = (QStandardItemModel*)ui->tvPlayerList->model();
    int curcount = model->rowCount();
    int newcount = (int)players.size();

    if (curcount > newcount)
        model->removeRows(newcount, curcount - newcount);
    else
        while (model->rowCount() < newcount)
        {
            QList<QStandardItem*> row;
            for (int i = 0; i < 4; i++) row.append(new QStandardItem());
            model->appendRow(row);
        }

    for (int i = 0; i < newcount; i++)
    {
        const auto& p = players[i];

        model->item(i,0)->setText(QString("%1/%2").arg(p.ID+1).arg(maxplayers));
        model->item(i,1)->setText(QString::fromLatin1(p.Name));
        model->item(i,2)->setText(p.Connected ? "Connected" : "Disconnected");
        model->item(i,3)->setText(p.Ping > 0 ? QString("%1 ms").arg(p.Ping) : "-");
    }

    // Update title with live count
    setWindowTitle(QString("Game session (%1/%2 players) - KHWaterMelonMix")
                   .arg(newcount).arg(maxplayers));
}
