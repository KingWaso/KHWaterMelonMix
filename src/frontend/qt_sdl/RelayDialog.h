/*
    Copyright 2016-2026 melonDS team / KHWaterMelonMix

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

#ifndef RELAYDIALOG_H
#define RELAYDIALOG_H

#include <QDialog>
#include <QTimer>
#include "types.h"

namespace Ui
{
    class RelayHostDialog;
    class RelayClientDialog;
    class RelaySessionDialog;
}

// ── "Host Game" dialog ───────────────────────────────────────────────────────
class RelayHostDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RelayHostDialog(QWidget* parent);
    ~RelayHostDialog();

    static RelayHostDialog* openDlg(QWidget* parent)
    {
        RelayHostDialog* dlg = new RelayHostDialog(parent);
        dlg->open();
        return dlg;
    }

private slots:
    void done(int r) override;

private:
    Ui::RelayHostDialog* ui;
};

// ── "Join Game" dialog ───────────────────────────────────────────────────────
class RelayClientDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RelayClientDialog(QWidget* parent);
    ~RelayClientDialog();

    static RelayClientDialog* openDlg(QWidget* parent)
    {
        RelayClientDialog* dlg = new RelayClientDialog(parent);
        dlg->open();
        return dlg;
    }

private slots:
    void done(int r) override;

private:
    Ui::RelayClientDialog* ui;
};

// ── In-session player list ───────────────────────────────────────────────────
class RelaySessionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RelaySessionDialog(QWidget* parent);
    ~RelaySessionDialog();

    static RelaySessionDialog* openDlg(QWidget* parent)
    {
        RelaySessionDialog* dlg = new RelaySessionDialog(parent);
        dlg->show();
        return dlg;
    }

private slots:
    void on_btnLeave_clicked();
    void done(int r) override;
    void doUpdatePlayerList();

private:
    Ui::RelaySessionDialog* ui;
    QTimer* refreshTimer;
};

#endif // RELAYDIALOG_H
