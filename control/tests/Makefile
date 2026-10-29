# test_reply.py runs the reply program in a PIO emulator; the firmware build regenerates bus.pio.h
test: test_control
	./test_control
	uvx --with rp2040-pio-emulator==0.88.0 python -I test_reply.py ../bus.pio.h

test_control: test_control.cpp ../spa.cpp ../spa.h ../schedule.cpp ../schedule.h
	$(CXX) -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I.. -o $@ test_control.cpp ../spa.cpp ../schedule.cpp

.PHONY: test
