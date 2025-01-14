/*
Copyright (C) 2016 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "pcaprxstats.h"

#include "pcapextra.h"
#include "../common/sign.h"

#define Xnotify qWarning // FIXME

PcapRxStats::PcapRxStats(const char *device, StreamStats &portStreamStats, int id)
    : streamStats_(portStreamStats)
{
    device_ = QString::fromLatin1(device);
    stop_ = false;
    state_ = kNotStarted;
    isDirectional_ = true;

    handle_ = NULL;

    id_ = id;
}

pcap_t* PcapRxStats::handle()
{
    return handle_;
}

void PcapRxStats::run()
{
    int flags = PCAP_OPENFLAG_PROMISCUOUS;
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    struct bpf_program bpf;
    const int optimize = 1;
    QString capture_filter = QString("(ether[len - 4:4] == 0x%1)").arg(
            SignProtocol::magic(), 0, BASE_HEX);
    // XXX: Exclude ICMP packets which contain an embedded signed packet
    //      For now we check upto 4 vlan tags
    capture_filter.append(
        "and not ("
            "icmp or "
            "(vlan and icmp) or "
            "(vlan and icmp) or "
            "(vlan and icmp) or "
            "(vlan and icmp) "
        ")");

    qDebug("In %s", __PRETTY_FUNCTION__);

    handle_ = pcap_open_live(qPrintable(device_), 65535,
                    flags, 100 /* ms */, errbuf);
    if (handle_ == NULL) {
        if (flags && QString(errbuf).contains("promiscuous")) {
            Xnotify("Unable to set promiscuous mode on <%s> - "
                    "stream stats rx will not work", qPrintable(device_));
            goto _exit;
        }
        else {
            Xnotify("Unable to open <%s> [%s] - stream stats rx will not work",
                    qPrintable(device_), errbuf);
            goto _exit;
        }
    }

#ifdef Q_OS_WIN32
    // pcap_setdirection() API is not supported in Windows.
    // NOTE: WinPcap 4.1.1 and above exports a dummy API that returns -1
    // but since we would like to work with previous versions of WinPcap
    // also, we assume the API does not exist
    isDirectional_ = false;
#else
    if (pcap_setdirection(handle_, PCAP_D_IN) < 0) {
        qDebug("RxStats: Error setting IN direction %s: %s\n",
                qPrintable(device_), pcap_geterr(handle_));
        isDirectional_ = false;
    }
#endif

    if (pcap_compile(handle_, &bpf, qPrintable(capture_filter),
                     optimize, 0) < 0) {
        qWarning("%s: error compiling filter: %s", qPrintable(device_),
                pcap_geterr(handle_));
        goto _skip_filter;
    }

    if (pcap_setfilter(handle_, &bpf) < 0) {
        qWarning("%s: error setting filter: %s", qPrintable(device_),
                pcap_geterr(handle_));
        goto _skip_filter;
    }

_skip_filter:
    memset(&lastPcapStats_, 0, sizeof(lastPcapStats_));
    PcapSession::preRun();
    state_ = kRunning;
    while (1) {
        int ret;
        struct pcap_pkthdr *hdr;
        const uchar *data;

        ret = pcap_next_ex(handle_, &hdr, &data);
        switch (ret) {
            case 1: {
                uint guid;
                if (SignProtocol::packetGuid(data, hdr->caplen, &guid)) {
                    streamStats_[guid].rx_pkts++;
                    streamStats_[guid].rx_bytes += hdr->caplen;
                }
                break;
            }
            case 0:
                // timeout: just go back to the loop
                break;
            case -1:
                qWarning("%s: error reading packet (%d): %s",
                        __PRETTY_FUNCTION__, ret, pcap_geterr(handle_));
                break;
            case -2:
                qDebug("Loop/signal break or some other error");
                break;
            default:
                qWarning("%s: Unexpected return value %d", __PRETTY_FUNCTION__,
                        ret);
                stop_ = true;
        }

        if (stop_) {
            qDebug("user requested rxstats stop");
            break;
        }
    }
    PcapSession::postRun();
    pcap_close(handle_);
    handle_ = NULL;
    stop_ = false;

_exit:
    state_ = kFinished;
}

bool PcapRxStats::start()
{
    if (state_ == kRunning) {
        qWarning("RxStats start requested but is already running!");
        goto _exit;
    }

    state_ = kNotStarted;
    PcapSession::start();

    while (state_ == kNotStarted)
        QThread::msleep(10);
_exit:
    return true;
}

bool PcapRxStats::stop()
{
    if (state_ == kRunning) {
        stop_ = true;
        PcapSession::stop(handle_);
        while (state_ == kRunning)
            QThread::msleep(10);
    }
    else
        qWarning("RxStats stop requested but is not running!");

    return true;
}

bool PcapRxStats::isRunning()
{
    return (state_ == kRunning);
}

bool PcapRxStats::isDirectional()
{
    return isDirectional_;
}

// XXX: Implemented as reset on read
QString PcapRxStats::debugStats()
{
    QString dbgStats;

#ifdef Q_OS_WIN32
    static_assert(sizeof(struct pcap_stat) == 6*sizeof(uint),
                "pcap_stat has less or more than 6 values");
    int size;
    struct pcap_stat incPcapStats;
    struct pcap_stat *pcapStats = pcap_stats_ex(handle_, &size);
    if (pcapStats && (uint(size) >= 6*sizeof(uint))) {
        incPcapStats.ps_recv = pcapStats->ps_recv - lastPcapStats_.ps_recv;
        incPcapStats.ps_drop = pcapStats->ps_drop - lastPcapStats_.ps_drop;
        incPcapStats.ps_ifdrop = pcapStats->ps_ifdrop - lastPcapStats_.ps_ifdrop;
        incPcapStats.ps_capt = pcapStats->ps_capt - lastPcapStats_.ps_capt;
        incPcapStats.ps_sent = pcapStats->ps_sent - lastPcapStats_.ps_sent;
        incPcapStats.ps_netdrop = pcapStats->ps_netdrop - lastPcapStats_.ps_netdrop;
        dbgStats = QString("recv: %1 drop: %2 ifdrop: %3 "
                           "capt: %4 sent: %5 netdrop: %6")
                        .arg(incPcapStats.ps_recv)
                        .arg(incPcapStats.ps_drop)
                        .arg(incPcapStats.ps_ifdrop)
                        .arg(incPcapStats.ps_capt)
                        .arg(incPcapStats.ps_sent)
                        .arg(incPcapStats.ps_netdrop);
        lastPcapStats_ = *pcapStats;
    } else {
        dbgStats = QString("error reading pcap stats: %1")
                        .arg(pcap_geterr(handle_));
    }
#else
    struct pcap_stat pcapStats;
    struct pcap_stat incPcapStats;

    int ret = pcap_stats(handle_, &pcapStats);
    if (ret == 0) {
        incPcapStats.ps_recv = pcapStats.ps_recv - lastPcapStats_.ps_recv;
        incPcapStats.ps_drop = pcapStats.ps_drop - lastPcapStats_.ps_drop;
        incPcapStats.ps_ifdrop = pcapStats.ps_ifdrop - lastPcapStats_.ps_ifdrop;
        dbgStats = QString("recv: %1 drop: %2 ifdrop: %3")
                        .arg(incPcapStats.ps_recv)
                        .arg(incPcapStats.ps_drop)
                        .arg(incPcapStats.ps_ifdrop);
        lastPcapStats_ = pcapStats;
    } else {
        dbgStats = QString("error reading pcap stats: %1")
                        .arg(pcap_geterr(handle_));
    }
#endif

    return dbgStats;
}
