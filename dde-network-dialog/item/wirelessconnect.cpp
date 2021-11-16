/*
 * Copyright (C) 2011 ~ 2021 Deepin Technology Co., Ltd.
 *
 * Author:     caixiangrong <caixiangrong@uniontech.com>
 *
 * Maintainer: caixiangrong <caixiangrong@uniontech.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "wirelessconnect.h"

#include <wirelessdevice.h>

#include <NetworkManagerQt/AccessPoint>
#include <NetworkManagerQt/Settings>
#include <NetworkManagerQt/WirelessSecuritySetting>
#include <NetworkManagerQt/Security8021xSetting>
#include <NetworkManagerQt/WirelessSetting>
#include <NetworkManagerQt/Ipv4Setting>
#include <NetworkManagerQt/Ipv6Setting>
#include <NetworkManagerQt/Utils>

#include <pwd.h>

#define LIGHTDM_USER "lightdm"

using namespace dde::network;
using namespace NetworkManager;

WirelessConnect::WirelessConnect(QObject *parent, dde::network::WirelessDevice *device, dde::network::AccessPoints *ap)
    : QObject(parent)
    , m_device(device)
    , m_accessPoint(ap)
{
}

WirelessConnect::~WirelessConnect()
{
}

void WirelessConnect::setSsid(const QString &ssid)
{
    m_ssid = ssid;
    m_connectionSettings.clear();
}

bool WirelessConnect::passwordIsValid(const QString &password)
{
    WirelessSecuritySetting::Ptr wsSetting = m_connectionSettings->setting(Setting::SettingType::WirelessSecurity).staticCast<WirelessSecuritySetting>();
    WirelessSecuritySetting::KeyMgmt keyMgmt = wsSetting->keyMgmt();
    if (keyMgmt == WirelessSecuritySetting::KeyMgmt::Wep) {
        return wepKeyIsValid(password, WirelessSecuritySetting::WepKeyType::Passphrase);
    }
    return wpaPskIsValid(password);
}

WirelessSecuritySetting::KeyMgmt WirelessConnect::getKeyMgmtByAp(dde::network::AccessPoints *ap)
{
    if (nullptr == ap) {
        return WirelessSecuritySetting::WpaPsk;
    }
    AccessPoint::Ptr nmAp(new AccessPoint(ap->path()));
    AccessPoint::Capabilities capabilities = nmAp->capabilities();
    AccessPoint::WpaFlags wpaFlags = nmAp->wpaFlags();
    AccessPoint::WpaFlags rsnFlags = nmAp->rsnFlags();

    WirelessSecuritySetting::KeyMgmt keyMgmt = WirelessSecuritySetting::KeyMgmt::WpaNone;

    if (capabilities.testFlag(AccessPoint::Capability::Privacy) && !wpaFlags.testFlag(AccessPoint::WpaFlag::KeyMgmtPsk) && !wpaFlags.testFlag(AccessPoint::WpaFlag::KeyMgmt8021x)) {
        keyMgmt = WirelessSecuritySetting::KeyMgmt::Wep;
    }

    if (wpaFlags.testFlag(AccessPoint::WpaFlag::KeyMgmtPsk) || rsnFlags.testFlag(AccessPoint::WpaFlag::KeyMgmtPsk)) {
        keyMgmt = WirelessSecuritySetting::KeyMgmt::WpaPsk;
    }

    if (wpaFlags.testFlag(AccessPoint::WpaFlag::KeyMgmt8021x) || rsnFlags.testFlag(AccessPoint::WpaFlag::KeyMgmt8021x)) {
        keyMgmt = WirelessSecuritySetting::KeyMgmt::WpaEap;
    }
    return keyMgmt;
}

void WirelessConnect::initConnection()
{
    // 登录界面(用户为lightdm)密码保存为储存所有用户密码
    struct passwd *userInfo = getpwuid(getuid());
    Setting::SecretFlagType defaultSecretFalg = (QString(userInfo->pw_name) == LIGHTDM_USER) ? Setting::None : Setting::AgentOwned;

    NetworkManager::Connection::Ptr conn;
    const QList<WirelessConnection *> lstConnections = m_device->items();
    for (auto item : lstConnections) {
        if (item->connection()->ssid() != m_ssid)
            continue;

        QString uuid = item->connection()->uuid();
        if (!uuid.isEmpty()) {
            conn = findConnectionByUuid(uuid);
            if (!conn.isNull() && conn->isValid()) {
                m_connectionSettings = conn->settings();

                Setting::SettingType sType = Setting::SettingType::WirelessSecurity;
                WirelessSecuritySetting::KeyMgmt keyMgmt = m_connectionSettings->setting(sType).staticCast<WirelessSecuritySetting>()->keyMgmt();
                if (keyMgmt != WirelessSecuritySetting::KeyMgmt::WpaNone
                    && keyMgmt != WirelessSecuritySetting::KeyMgmt::Unknown) {
                    if (keyMgmt == WirelessSecuritySetting::KeyMgmt::WpaEap) {
                        sType = Setting::SettingType::Security8021x;
                    }
                    qInfo() << "setting:" << m_connectionSettings->setting(sType)->typeAsString(sType);
                    QDBusPendingReply<NMVariantMapMap> reply;
                    reply = conn->secrets(m_connectionSettings->setting(sType)->name());
                    reply.waitForFinished();
                    if (reply.isError() || !reply.isValid()) {
                        qDebug() << "get secrets error for connection:" << reply.error();
                    }

                    QSharedPointer<WirelessSecuritySetting> setting = m_connectionSettings->setting(sType).staticCast<WirelessSecuritySetting>();
                    setting->secretsFromMap(reply.value().value(setting->name()));
                }
                // 检查密码 密码有效则继续(有可能有多个配置)
                QString password;
                hasPassword(password);
                if (!password.isEmpty()) {
                    break;
                }
            }
        }
    }
    //　没连接过的需要新建连接
    if (m_connectionSettings.isNull()) {
        m_connectionSettings = QSharedPointer<ConnectionSettings>(new ConnectionSettings(ConnectionSettings::ConnectionType::Wireless));
        // 创建uuid
        QString uuid = m_connectionSettings->createNewUuid();
        while (findConnectionByUuid(uuid)) {
            qint64 second = QDateTime::currentDateTime().toSecsSinceEpoch();
            uuid.replace(24, QString::number(second).length(), QString::number(second));
        }
        m_connectionSettings->setUuid(uuid);
        m_connectionSettings->setId(m_ssid);
        if (!m_accessPoint) {
            m_connectionSettings->setting(Setting::SettingType::Wireless).staticCast<WirelessSetting>()->setHidden(true);
        }
        qInfo() << "create connect:" << m_ssid << uuid << m_accessPoint;
        if (m_accessPoint) {
            m_connectionSettings->setting(Setting::Security8021x).staticCast<Security8021xSetting>()->setPasswordFlags(Setting::AgentOwned);
            WirelessSecuritySetting::Ptr wsSetting = m_connectionSettings->setting(Setting::WirelessSecurity).dynamicCast<WirelessSecuritySetting>();
            WirelessSecuritySetting::KeyMgmt keyMgmt = getKeyMgmtByAp(m_accessPoint);
            if (keyMgmt != WirelessSecuritySetting::KeyMgmt::WpaNone) {
                wsSetting->setKeyMgmt(keyMgmt);
                if (keyMgmt == WirelessSecuritySetting::KeyMgmt::Wep) {
                    wsSetting->setWepKeyFlags(defaultSecretFalg);
                } else if (keyMgmt == WirelessSecuritySetting::KeyMgmt::WpaPsk) {
                    wsSetting->setPskFlags(defaultSecretFalg);
                }
                wsSetting->setInitialized(true);
            }
        }
        WirelessSetting::Ptr wirelessSetting = m_connectionSettings->setting(Setting::Wireless).dynamicCast<WirelessSetting>();
        wirelessSetting->setSsid(m_ssid.toUtf8());
        wirelessSetting->setMacAddress(QByteArray());
        wirelessSetting->setMtu(0);
        wirelessSetting->setInitialized(true);
    }
}

void WirelessConnect::setPassword(const QString &password)
{
    WirelessSecuritySetting::Ptr wsSetting = m_connectionSettings->setting(Setting::SettingType::WirelessSecurity).staticCast<WirelessSecuritySetting>();
    WirelessSecuritySetting::KeyMgmt keyMgmt = wsSetting->keyMgmt();
    if (keyMgmt == WirelessSecuritySetting::KeyMgmt::Wep) {
        wsSetting->setWepKey0(password);
    } else if (keyMgmt == WirelessSecuritySetting::KeyMgmt::WpaPsk) {
        wsSetting->setPsk(password);
    }
    wsSetting->setInitialized(true);
}

/**
 * @brief WirelessConnect::hasPassword
 * @param password [out] 已保存的密码
 * @return 是否需要密码
 */
bool WirelessConnect::hasPassword(QString &password)
{
    if (m_accessPoint && m_accessPoint->secured()) {
        WirelessSecuritySetting::Ptr wsSetting = m_connectionSettings->setting(Setting::SettingType::WirelessSecurity).staticCast<WirelessSecuritySetting>();
        WirelessSecuritySetting::KeyMgmt keyMgmt = wsSetting->keyMgmt();

        switch (keyMgmt) {
        case WirelessSecuritySetting::KeyMgmt::Wep: {
            password = wsSetting->wepKey0();
            return true;
        }
        case WirelessSecuritySetting::KeyMgmt::WpaPsk: {
            password = wsSetting->psk();
            return true;
        }
        default:
            break;
        }
        return true;
    }

    if (!m_accessPoint || (m_accessPoint && m_accessPoint->secured())) {
        return true;
    }
    return false;
}

void WirelessConnect::connectNetwork()
{
    initConnection();
    // 隐藏网络先尝试无密码连接
    if (m_accessPoint) {
        QString password;
        if ((hasPassword(password) && password.isEmpty())) {
            emit passwordError(QString());
            return;
        }
    }
    activateConnection();
}

void WirelessConnect::connectNetworkPassword(const QString password)
{
    initConnection();
    setPassword(password);
    activateConnection();
}

void WirelessConnect::activateConnection()
{
    QString connPath;
    QString connectionUuid = m_connectionSettings->uuid();
    NetworkManager::Connection::Ptr conn = findConnectionByUuid(connectionUuid);
    QString accessPointPath;
    if (m_accessPoint) {
        accessPointPath = m_accessPoint->path();
    }
    if (conn.isNull()) {
        qInfo() << "addAndActivateConnection" << connPath << m_device->path() << accessPointPath;
        addAndActivateConnection(m_connectionSettings->toMap(), m_device->path(), accessPointPath);
        return;
    }
    QDBusPendingReply<> reply;
    reply = conn->update(m_connectionSettings->toMap());
    reply.waitForFinished();
    if (reply.isError()) {
        qInfo() << "error occurred while updating the connection" << reply.error();
        return;
    }
    connPath = conn->path();
    NetworkManager::activateConnection(connPath, m_device->path(), accessPointPath);
}

void WirelessConnect::getoldPassword()
{
    initConnection();
    QString password;
    hasPassword(password);
    emit passwordError(password);
}