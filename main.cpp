// main.cpp — scaffold. Proves the toolchain links before real code lands.
#include "ComputerCard.h"

#include "hardware/vreg.h"
#include "pico/stdlib.h"

class Ocarina : public ComputerCard
{
public:
	Ocarina() {}
	virtual void __not_in_flash_func(ProcessSample)() override
	{
		AudioOut1(0);
		AudioOut2(0);
	}
};

int main()
{
	vreg_set_voltage(VREG_VOLTAGE_1_15);
	sleep_ms(2);
	set_sys_clock_khz(192000, true);

	static Ocarina card;
	card.EnableNormalisationProbe();
	card.Run();
}
