CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread -I.
MYSQL_CFLAGS := $(shell mysql_config --cflags)
MYSQL_LIBS := $(shell mysql_config --libs)

TARGET := server
MYSQL_TEST := mysql_pool_test

SRCS := app/main.cpp webserver/webserver.cpp http/http_conn.cpp http/auth.cpp thread_pool/thread_pool.cpp logger/logger.cpp db/sql_connection_pool.cpp
MYSQL_TEST_SRCS := tests/mysql_pool_test.cpp db/sql_connection_pool.cpp logger/logger.cpp

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(MYSQL_CFLAGS) -o $(TARGET) $(SRCS) $(MYSQL_LIBS)

$(MYSQL_TEST): $(MYSQL_TEST_SRCS)
	$(CXX) $(CXXFLAGS) $(MYSQL_CFLAGS) -o $(MYSQL_TEST) $(MYSQL_TEST_SRCS) $(MYSQL_LIBS)

clean:
	rm -f $(TARGET) $(MYSQL_TEST) *.o */*.o
