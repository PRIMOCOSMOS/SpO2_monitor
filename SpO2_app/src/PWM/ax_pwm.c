

/***************************** Include Files *******************************/
#include "ax_pwm.h"

void set_pwm_frequency(unsigned int BaseAddress,int refclk_freq,float freq)
{
	AX_PWM_mWriteReg(BaseAddress,0,(unsigned int)((4294967296.0/refclk_freq) * freq));
}
void set_pwm_duty(unsigned int BaseAddress,float duty)
{
    /* duty 为 0.0~100.0 百分比，防止溢出 */
    if(duty > 100.0f) duty = 100.0f;
    if(duty < 0.0f)   duty = 0.0f;
    float fraction = duty / 100.0f;
    AX_PWM_mWriteReg(BaseAddress, 4, (unsigned int)(4294967296.0 * fraction));
}

/************************** Function Definitions ***************************/
