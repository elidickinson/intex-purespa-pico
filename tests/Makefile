SRC = ../components/purespa

test: test_protocol
	./test_protocol

test_protocol: test_protocol.cpp $(SRC)/protocol.cpp $(SRC)/protocol.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I$(SRC) -o $@ test_protocol.cpp $(SRC)/protocol.cpp

.PHONY: test
