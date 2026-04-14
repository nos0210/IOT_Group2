#ifndef ADC_DAC_CONFIG_H
#define ADC_DAC_CONFIG_H


#define SLIDING_WINDOW_SIZE     10

#define LOWPASS_ALPHA           0.3f

#define AVERAGE_FILTER_SIZE     10


#define ADC_CHANNEL             ADC_CHANNEL_0

//ADC Resolution (12-bit = 0-4095)
#define ADC_BITWIDTH            ADC_BITWIDTH_12

//ADC Attenuation (12dB = 0-5V full range) 
#define ADC_ATTEN               ADC_ATTEN_DB_12


#define DAC_CHANNEL             DAC_CHAN_0


#define ADC_SAMPLE_INTERVAL_MS  50


#define ADC_TASK_STACK_SIZE     4096


#define ADC_TASK_PRIORITY       5


#define LOG_PRINT_INTERVAL      50

#define SELECTED_FILTER         1  


#define ADC_TO_DAC_RATIO        16


#define DEBUG_PRINT_ALL         0

#define DEBUG_INIT_INFO         1


#define DEBUG_VERIFY_DATA       1

#endif
