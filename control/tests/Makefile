test: test_control
	./test_control

test_control: test_control.cpp ../spa.cpp ../spa.h ../schedule.cpp ../schedule.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -o $@ test_control.cpp ../spa.cpp ../schedule.cpp

.PHONY: test
