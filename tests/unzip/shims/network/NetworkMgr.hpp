#pragma once
struct TestNetworkMgr {
    bool connected = false;
    bool IsConnected() const { return connected; }
};
inline TestNetworkMgr NetworkMgr;
