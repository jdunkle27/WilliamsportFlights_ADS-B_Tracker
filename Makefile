CXX ?= g++
PKG_CONFIG ?= pkg-config
TARGET ?= adsb_tracker
SOURCE := adsb_tracker.cpp

RTLSDR_CFLAGS := $(shell $(PKG_CONFIG) --cflags librtlsdr 2>/dev/null)
CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
RTLSDR_LIBS := $(shell $(PKG_CONFIG) --libs librtlsdr 2>/dev/null || printf '%s' '-lrtlsdr')
CURL_LIBS := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || printf '%s' '-lcurl')

CPPFLAGS += $(RTLSDR_CFLAGS) $(CURL_CFLAGS)
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra
LDLIBS += $(RTLSDR_LIBS) $(CURL_LIBS) -pthread

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)