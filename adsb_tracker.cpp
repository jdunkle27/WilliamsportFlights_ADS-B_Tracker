#include <rtl-sdr.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>
#include <cstdlib>

namespace {
constexpr uint32_t DEFAULT_FREQ = 1090000000U;
constexpr uint32_t SAMPLE_RATE = 2000000U;
constexpr size_t READ_LEN = 262144;
constexpr double PI = 3.14159265358979323846;

// Structure to hold aircraft data
struct Aircraft {
    std::string hex;
    std::string tailnumber;
    double lat = NAN, lon = NAN;
    double velocity = NAN;
    int altitude = 0;
    bool has_altitude = false;
    bool has_velocity = false;
    bool has_position = false;
    int even_lat = 0, even_lon = 0, odd_lat = 0, odd_lon = 0;
    std::chrono::steady_clock::time_point even_time{}, odd_time{}, last_seen{};
};

// Extract a specified number of bits from a message starting at a given bit position
uint32_t getBits(const uint8_t* msg, int start, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; ++i) {
        int bit = start + i;
        v = (v << 1) | ((msg[bit / 8] >> (7 - (bit % 8))) & 1);
    }
    return v;
}

// Convert a 24-bit integer to a 6-digit uppercase hexadecimal string
std::string hexString(uint32_t v) {
    std::ostringstream s;
    s << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << v;
    return s.str();
}

// Compute the CRC24 of a message with a given number of bits
uint32_t crc24(const uint8_t* msg, int bits) {
    uint32_t rem = 0;
    for (int i = 0; i < bits; ++i) {
        uint32_t bit = (msg[i / 8] >> (7 - (i % 8))) & 1;
        uint32_t top = (rem >> 23) & 1;
        rem = (rem << 1) & 0xFFFFFF;
        if (top ^ bit) rem ^= 0xFFF409;
    }
    return rem & 0xFFFFFF;
}

// Check if a Type Code 17 message has a valid CRC
bool validDF17(const uint8_t* msg) {
    return crc24(msg, 112) == 0;
}

// Decode the tail number from a Type Code 1-4 message
std::string decodetailnumber(const uint8_t* msg) {
    static const char charset[65] =
        "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
    std::string out;
    for (int i = 0; i < 8; ++i) {
        unsigned c = getBits(msg, 40 + i * 6, 6);
        char ch = (c < 64) ? charset[c] : ' ';
        if (ch == '?') ch = ' ';
        out.push_back(ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return "";
    return out.substr(first);
}

// Decode altitude in feet from a Type Code 9-18 or 20-22 message
int decodeAltitudeFeet(const uint8_t* msg) {
    uint32_t ac12 = getBits(msg, 40, 12);
    if (ac12 == 0) return 0;
    if (ac12 & 0x10) {
        int n = static_cast<int>(((ac12 & 0xFE0) >> 1) | (ac12 & 0x0F));
        return n * 25 - 1000;
    }
    return 0;
}

// Compute the number of latitude zones (NL) for a given latitude in degrees
double cprNL(double lat) {
    if (lat < 0) lat = -lat;
    if (lat >= 87.0) return 1.0;
    double c = std::cos(PI * lat / 180.0);
    double denom = c * c;
    if (denom <= 0.0) return 1.0;
    double x = 1.0 - (1.0 - std::cos(PI / 30.0)) / denom;
    if (x < -1.0) x = -1.0;
    if (x > 1.0) x = 1.0;
    return std::floor(2.0 * PI / std::acos(x));
}


// Decode global CPR position from even and odd messages and update the aircraft data
bool decodeGlobalCPR(Aircraft& a) {
    using clock = std::chrono::steady_clock;
    if (a.even_time.time_since_epoch().count() == 0 ||
        a.odd_time.time_since_epoch().count() == 0) return false;
    auto delta = a.even_time > a.odd_time ? a.even_time - a.odd_time : a.odd_time - a.even_time;
    if (delta > std::chrono::seconds(10)) return false;

    constexpr double scale = 131072.0;
    double lat0 = a.even_lat / scale, lat1 = a.odd_lat / scale;
    double j = std::floor(59.0 * lat0 - 60.0 * lat1 + 0.5);
    double rlat0 = 6.0 * (std::fmod(j, 60.0) + lat0);
    double rlat1 = (360.0 / 59.0) * (std::fmod(j, 59.0) + lat1);
    if (rlat0 >= 270.0) rlat0 -= 360.0;
    if (rlat1 >= 270.0) rlat1 -= 360.0;
    if (rlat0 < -90.0 || rlat0 > 90.0 || rlat1 < -90.0 || rlat1 > 90.0) return false;

    double nl0 = cprNL(rlat0), nl1 = cprNL(rlat1);
    if (nl0 != nl1) return false;
    bool use_odd = a.odd_time > a.even_time;
    double lat = use_odd ? rlat1 : rlat0;
    int ni = static_cast<int>(std::max(1.0, nl0 - (use_odd ? 1.0 : 0.0)));
    double m = std::floor(a.even_lon * (nl0 - 1.0) / scale -
                          a.odd_lon * nl0 / scale + 0.5);
    double lon_cpr = use_odd ? a.odd_lon / scale : a.even_lon / scale;
    double lon = (360.0 / ni) * (std::fmod(m, static_cast<double>(ni)) + lon_cpr);
    if (lon > 180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
    a.lat = lat;
    a.lon = lon;
    a.has_position = true;
    return true;
}

// Decode velocity information from a Type Code 19 message and update the aircraft data
void decodeVelocity(Aircraft& a, const uint8_t* msg) {
    int tc = static_cast<int>(getBits(msg, 32, 5));
    if (tc != 19) return;
    int subtype = static_cast<int>(getBits(msg, 37, 3));
    if (subtype == 1 || subtype == 2) {
        int ew_sign = static_cast<int>(getBits(msg, 45, 1));
        int ew = static_cast<int>(getBits(msg, 46, 10));
        int ns_sign = static_cast<int>(getBits(msg, 56, 1));
        int ns = static_cast<int>(getBits(msg, 57, 10));
        if (ew == 0 || ns == 0) return;
        double factor = subtype == 2 ? 4.0 : 1.0;
        double east = (ew - 1) * factor * (ew_sign ? -1.0 : 1.0);
        double north = (ns - 1) * factor * (ns_sign ? -1.0 : 1.0);
        a.velocity = std::sqrt(east * east + north * north);
        a.has_velocity = true;
    } else if (subtype == 3 || subtype == 4) {
        int heading_status = static_cast<int>(getBits(msg, 45, 1));
        int heading = static_cast<int>(getBits(msg, 46, 10));
        int airspeed = static_cast<int>(getBits(msg, 57, 10));
        if (heading_status && airspeed > 0) {
            a.velocity = airspeed - 1;
            a.has_velocity = true;
        }
        (void)heading;
    }
}

// Update aircraft data in the airspace map based on the received message
void updateAircraft(const uint8_t* msg, std::map<std::string, Aircraft>& airspace) {
    uint32_t icao = getBits(msg, 8, 24);
    std::string hex = hexString(icao);
    Aircraft& a = airspace[hex];
    a.hex = hex;
    a.last_seen = std::chrono::steady_clock::now();

    int tc = static_cast<int>(getBits(msg, 32, 5));
    if (tc >= 1 && tc <= 4) {
        std::string cs = decodetailnumber(msg);
        if (!cs.empty()) a.tailnumber = cs;
    } else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        int alt = decodeAltitudeFeet(msg);
        if (alt != 0) {
            a.altitude = alt;
            a.has_altitude = true;
        }
        int odd = static_cast<int>(getBits(msg, 53, 1));
        int lat = static_cast<int>(getBits(msg, 54, 17));
        int lon = static_cast<int>(getBits(msg, 71, 17));
        if (odd) {
            a.odd_lat = lat; a.odd_lon = lon; a.odd_time = a.last_seen;
        } else {
            a.even_lat = lat; a.even_lon = lon; a.even_time = a.last_seen;
        }
        decodeGlobalCPR(a);
    } else if (tc == 19) {
        decodeVelocity(a, msg);
    }
}


// Convert a double to a string with fixed precision, or "N/A" if not finite
std::string val(double x, int precision = 4) {
    if (!std::isfinite(x)) return "N/A";
    std::ostringstream s;
    s << std::fixed << std::setprecision(precision) << x;
    return s.str();
}

// Print a table of aircraft data to the console
void printTable(const std::map<std::string, Aircraft>& airspace) {
    std::cout << "\033[2J\033[H";
    std::cout << "RTL-SDR ADS-B receiver | 1090 MHz | " << airspace.size()
              << " aircraft tracked\n\n";
    std::cout << std::left
              << std::setw(9) << "hex"
              << std::setw(18) << "tail number"
              << std::setw(13) << "latitude"
              << std::setw(14) << "longitude"
              << std::setw(13) << "altitude"
              << std::setw(12) << "velocity" << '\n';
    std::cout << std::string(79, '-') << '\n';
    for (const auto& kv : airspace) {
        const Aircraft& a = kv.second;
        std::string alt = a.has_altitude ? std::to_string(a.altitude) + " ft" : "N/A";
        std::string vel = a.has_velocity ? val(a.velocity, 0) + " kt" : "N/A";
        std::cout << std::left
                  << std::setw(9) << a.hex
                  << std::setw(18) << (a.tailnumber.empty() ? "N/A" : a.tailnumber)
                  << std::setw(13) << val(a.lat)
                  << std::setw(14) << val(a.lon)
                  << std::setw(13) << alt
                  << std::setw(12) << vel << '\n';
    }
    std::cout << "\nPress Ctrl+C to quit.\n" << std::flush;
}

// Convert interleaved I/Q samples to magnitudes
std::vector<uint8_t> magnitudes(const uint8_t* iq, size_t n) {
    std::vector<uint8_t> mag(n / 2);
    for (size_t i = 0; i < n; i += 2) {
        int I = static_cast<int>(iq[i]) - 127;
        int Q = static_cast<int>(iq[i + 1]) - 127;
        int m = static_cast<int>(std::sqrt(static_cast<double>(I * I + Q * Q)));
        mag[i / 2] = static_cast<uint8_t>(std::min(255, m));
    }
    return mag;
}

// Detect the preamble of an ADS-B message in the magnitude data
bool detectPreamble(const std::vector<uint8_t>& s, size_t i) {
    if (i + 16 >= s.size()) return false;
    int p0 = s[i], p1 = s[i + 2], p2 = s[i + 7], p3 = s[i + 9];
    int g1 = s[i + 1], g2 = s[i + 3], g3 = s[i + 5], g4 = s[i + 11], g5 = s[i + 13];
    int pulse = (p0 + p1 + p2 + p3) / 4;
    int gap = (g1 + g2 + g3 + g4 + g5) / 5;
    return pulse > 35 && pulse > gap * 1.65 && p0 > g1 * 1.35 &&
           p1 > g2 * 1.35 && p2 > g3 * 1.35 && p3 > g4 * 1.25;
}

// Decode an ADS-B frame from the magnitude data starting at a given index
bool decodeFrame(const std::vector<uint8_t>& s, size_t start, uint8_t msg[14], int& length) {
    if (start + 16 + 112 * 2 > s.size()) return false;
    uint8_t first = 0;
    for (int b = 0; b < 5; ++b) {
        size_t pos = start + 16 + b * 2;
        first = static_cast<uint8_t>((first << 1) | (s[pos] > s[pos + 1] ? 1 : 0));
    }
    int df = first;
    length = (df == 16 || df == 17 || df == 18 || df == 19) ? 112 : 56;
    if (start + 16 + static_cast<size_t>(length) * 2 > s.size()) return false;
    std::memset(msg, 0, 14);
    for (int b = 0; b < length; ++b) {
        size_t pos = start + 16 + b * 2;
        int bit = s[pos] > s[pos + 1] ? 1 : 0;
        msg[b / 8] |= static_cast<uint8_t>(bit << (7 - (b % 8)));
    }
    return true;
}

// Convert the airspace data to a JSON string representation
std::string jsonOut(const std::map<std::string, Aircraft>& airspace) {
    std::ostringstream s;
    s << "[";
    bool first = true;
    for (const auto& kv : airspace) {
        const Aircraft& a = kv.second;
        if (!first) s << ",";
        s << "{"
          << "\"hex\":\"" << a.hex << "\","
          << "\"tailnumber\":\"" << (a.tailnumber.empty() ? "" : a.tailnumber) << "\","
          << "\"latitude\":" << (a.has_position ? val(a.lat) : "null") << ","
          << "\"longitude\":" << (a.has_position ? val(a.lon) : "null") << ","
          << "\"altitude\":" << (a.has_altitude ? std::to_string(a.altitude) : "null") << ","
          << "\"velocity\":" << (a.has_velocity ? val(a.velocity, 0) : "null")
          << "}";
        first = false;
    }
    s << "]";

    return s.str();
}

bool toFastAPI(const std::string & json){
    CURL *curl = curl_easy_init();
    if (!curl) {
        std::cerr << "CURL failed to start." << std::endl;
        return false;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char curl_error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:8000/aircraft");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    
    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    } else {
        std::cerr << "FastAPI request failed: "
                  << (curl_error[0] ? curl_error : curl_easy_strerror(res)) << '\n';
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res == CURLE_OK && (status_code < 200 || status_code >= 300)) {
        std::cerr << "FastAPI returned HTTP " << status_code << ".\n";
    }

    return (res == CURLE_OK && status_code >= 200 && status_code < 300);
}

} // end namespace


int main(int argc, char** argv) {

    //Check for arguments for API use or local use
    bool web_mode = false;
    bool view_mode = false;
    uint32_t frequency = DEFAULT_FREQ;
    int gain_tenth_db = 0;

    int positional_arg = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--web") web_mode = true;
        else if (arg == "--view") view_mode = true;
        else if (positional_arg == 0) {
            frequency = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
            ++positional_arg;
        } else if (positional_arg == 1) {
            gain_tenth_db = std::atoi(argv[i]);
            ++positional_arg;
        }
    }
    if(!web_mode && !view_mode) {
        view_mode = true;
    }


    int count = rtlsdr_get_device_count();
    if (count == 0) {
        std::cerr << "RTL-SDR not found. Check connection.\n";
        return 1;
    }
    rtlsdr_dev_t* dev = nullptr;
    int rc = rtlsdr_open(&dev, 0);
    if (rc != 0 || !dev) {
        std::cerr << "Could not open RTL-SDR. Check device permissions.\n";
        return 1;
    }

    rtlsdr_set_sample_rate(dev, SAMPLE_RATE);
    rtlsdr_set_center_freq(dev, frequency);
    rtlsdr_set_tuner_gain_mode(dev, gain_tenth_db == 0 ? 0 : 1);
    if (gain_tenth_db != 0) rtlsdr_set_tuner_gain(dev, gain_tenth_db);
    rtlsdr_reset_buffer(dev);

    std::ostream& status = web_mode ? std::cerr : std::cout;
    status << "Using RTL-SDR at " << frequency / 1e6 << " MHz, sample rate "
           << SAMPLE_RATE / 1e6 << " MS/s\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    std::map<std::string, Aircraft> airspace;
    std::vector<uint8_t> iq(READ_LEN);
    size_t frames_since_print = 0;
    auto last_print = std::chrono::steady_clock::now();

    while (true) {
        int nread = 0;
        rc = rtlsdr_read_sync(dev, iq.data(), static_cast<int>(iq.size()), &nread);
        if (rc != 0 || nread <= 0) {
            std::cerr << "RTL-SDR read failed: " << rc << '\n';
            break;
        }
        nread &= ~1;
        auto mag = magnitudes(iq.data(), static_cast<size_t>(nread));

        for (size_t i = 0; i + 16 + 224 < mag.size(); ++i) {
            if (!detectPreamble(mag, i)) continue;
            uint8_t msg[14]{};
            int length = 0;
            if (!decodeFrame(mag, i, msg, length)) continue;
            int df = static_cast<int>(getBits(msg, 0, 5));
            if (df != 17 || length != 112) continue;
            if (!validDF17(msg)) continue;
            updateAircraft(msg, airspace);
            ++frames_since_print;
            i += 15;  }

        auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(2)) {
            for (auto it = airspace.begin(); it != airspace.end();) {
                if (now - it->second.last_seen > std::chrono::seconds(60))
                    it = airspace.erase(it);
                else ++it;
            }
            
            if (view_mode) {
                printTable(airspace);
            } else if (web_mode) {
                std::string json = jsonOut(airspace);
                if (toFastAPI(json)) {
                    std::cerr << "Data sent to FastAPI successfully.\n";
                }
                else {
                    std::cerr << "Failed to send data to FastAPI.\n";
                }
            }

            last_print = now;
            frames_since_print = 0;
        }
    }

    rtlsdr_close(dev);
    return 0;
}
