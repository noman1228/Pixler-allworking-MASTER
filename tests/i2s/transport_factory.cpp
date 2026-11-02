#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

class String : public std::string {
public:
    using std::string::string;
    using std::string::operator=;
    String(const std::string& value) : std::string(value) {}
    template<class T, typename std::enable_if<std::is_arithmetic<T>::value || std::is_enum<T>::value, int>::type = 0>
    String(T value) : std::string(std::to_string(value)) {}
};
using byte = uint8_t;
static const String CN_stars = "***";
static void logcon(const String&) {}
static void delay(unsigned) {}
#define F(value) value

#define ARDUINO_ARCH_ESP32
#define SUPPORT_OutputType_WS2811
#define SUPPORT_OutputType_MonsterPermanent
static bool IsBooting = true;
static const char* MN_09 = "Unknown type ";
static const char* MN_10 = " on ";
static const char* MN_11 = ".";
static const char* MN_12 = "Stopping ";
static const char* MN_13 = " on ";
static const char* MN_14 = " on ";
inline String operator+(const String& left, unsigned right) {
    return String(static_cast<const std::string&>(left) + std::to_string(right));
}
inline String operator+(const String& left, const char* right) {
    return String(static_cast<const std::string&>(left) + right);
}

enum class Transport { Disabled, Rmt, I2s, Uart };
struct c_OutputMgr {
    enum e_OutputType { OutputType_Disabled = 0, OutputType_WS2811 = 1, OutputType_MonsterPermanent = 16 };
    enum class OM_PortType_t { Rmt, Uart };
    struct DriverInfo_t {
        alignas(16) byte OutputDriver[256]{};
        bool OutputDriverInUse = false;
        unsigned DriverId = 0;
        int GpioPin = -1, PortId = -1;
        OM_PortType_t PortType = OM_PortType_t::Rmt;
    };
    bool BuildingNewConfig = false;
    void InstantiateNewOutputChannel(DriverInfo_t&, e_OutputType, bool = true);
};
struct Port { int GpioPin, PortId; c_OutputMgr::OM_PortType_t PortType; };
static Port OutputChannelIdToGpioAndPort[9] = {
    {0, 0, c_OutputMgr::OM_PortType_t::Rmt}, {1, 1, c_OutputMgr::OM_PortType_t::Rmt},
    {2, 2, c_OutputMgr::OM_PortType_t::Rmt}, {3, 3, c_OutputMgr::OM_PortType_t::Rmt},
    {4, 4, c_OutputMgr::OM_PortType_t::Rmt}, {5, 5, c_OutputMgr::OM_PortType_t::Rmt},
    {12, 6, c_OutputMgr::OM_PortType_t::Rmt}, {13, 7, c_OutputMgr::OM_PortType_t::Rmt},
    {17, 0, c_OutputMgr::OM_PortType_t::Uart}
};
#define OM_IS_UART (CurrentOutput.PortType == OM_PortType_t::Uart)
#define OM_IS_RMT (CurrentOutput.PortType == OM_PortType_t::Rmt)

struct c_OutputCommon {
    c_OutputMgr::e_OutputType type;
    Transport transport;
    unsigned starts = 0;
    c_OutputCommon(c_OutputMgr::e_OutputType t, Transport route) : type(t), transport(route) {}
    virtual ~c_OutputCommon() = default;
    auto GetOutputType() const { return type; }
    void GetDriverName(String& result) { result = "Host transport seam"; }
    void Begin() { ++starts; }
};
template<Transport Route>
struct Driver : c_OutputCommon {
    Driver(unsigned, int, int, c_OutputMgr::e_OutputType type) : c_OutputCommon(type, Route) {}
};
using c_OutputDisabled = Driver<Transport::Disabled>;
using c_OutputWS2811Rmt = Driver<Transport::Rmt>;
using c_OutputWS2811I2s = Driver<Transport::I2s>;
using c_OutputWS2811Uart = Driver<Transport::Uart>;
using c_OutputMonsterPermanentRmt = Driver<Transport::Rmt>;
using c_OutputMonsterPermanentUart = Driver<Transport::Uart>;
#include "factory_method.inc"

static unsigned checks = 0;
static void require(bool value, const char* reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
static c_OutputCommon& driver(c_OutputMgr::DriverInfo_t& port) {
    return *reinterpret_cast<c_OutputCommon*>(port.OutputDriver);
}
int main() {
    try {
        c_OutputMgr manager;
        for (unsigned id = 0; id < 9; ++id) {
            c_OutputMgr::DriverInfo_t port;
            port.DriverId = id;
            manager.InstantiateNewOutputChannel(port, c_OutputMgr::OutputType_Disabled);
            require(driver(port).transport == Transport::Disabled, "Disabled port acquired a transmitter");
            manager.InstantiateNewOutputChannel(port, c_OutputMgr::OutputType_WS2811);
            const Transport expected = id == 8 ? Transport::Uart :
#ifdef SUPPORT_PARALLEL_I2S
                Transport::I2s;
#else
                Transport::Rmt;
#endif
            require(driver(port).transport == expected && driver(port).starts == 1,
                    "WS2811 selected the wrong compile-time transport");
            require(port.DriverId == id && driver(port).type == c_OutputMgr::OutputType_WS2811,
                    "Transport choice changed a saved channel or protocol ID");
            manager.InstantiateNewOutputChannel(port, c_OutputMgr::OutputType_WS2811);
            require(driver(port).starts == 1, "Unchanged protocol restarted its driver");
            manager.InstantiateNewOutputChannel(port, c_OutputMgr::OutputType_MonsterPermanent);
            require(driver(port).transport == (id == 8 ? Transport::Uart : Transport::Rmt),
                    "Monster Permanent was incorrectly routed to I2S");
            require(driver(port).type == 16, "Monster saved protocol ID changed");
            manager.InstantiateNewOutputChannel(port, c_OutputMgr::OutputType_Disabled);
            require(driver(port).transport == Transport::Disabled, "Disabling did not remove its transmitter");
            driver(port).~c_OutputCommon();
        }
        std::cout << "Output transport factory passed: " << checks << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
