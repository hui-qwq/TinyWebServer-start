CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET := server
SRCS := main.cpp webserver.cpp http_conn.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET)
