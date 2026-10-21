//
// Created by LEGION on 2021/10/4.
//
#include "Device.h"
#include "UserTask.h"
#include "RemoteControl.h"
#include "Motor.h"

void aRGB_led_show(uint32_t aRGB){
    static uint8_t alpha;
    static uint16_t red,green,blue;
    alpha = (aRGB & 0xFF000000) >> 24;
    red = ((aRGB & 0x00FF0000) >> 16) * alpha;
    green = ((aRGB & 0x0000FF00) >> 8) * alpha;
    blue = ((aRGB & 0x000000FF) >> 0) * alpha;
            __HAL_TIM_SetCompare(&htim5, TIM_CHANNEL_1, blue);
            __HAL_TIM_SetCompare(&htim5, TIM_CHANNEL_2, green);
            __HAL_TIM_SetCompare(&htim5, TIM_CHANNEL_3, red);

}

/**
 * @brief 跑马灯函数，需周期性调用
 * @param period 跑马灯循环周期
 */
void aRGB_led_change(uint32_t period){
    static uint32_t aRGB_value = 0xffff0000;
    static uint32_t lastTick = 0;
    static float lastPhase = 0;
    uint32_t red,green,blue;
    red = ((aRGB_value & 0x00FF0000) >> 16) ;
    green = ((aRGB_value & 0x0000FF00) >> 8) ;
    blue = ((aRGB_value & 0x000000FF) >> 0) ;

    float interval = HAL_GetTick()- lastTick;
    if(lastTick == 0){//处理首次进入
        lastTick = HAL_GetTick();
        return;
    }
    interval = interval * 765/(float)period;
    //if(interval == 0)interval = 1;
    uint32_t phase =  (uint32_t)(interval+ lastPhase)%765;
    uint32_t group = phase/255;
    uint32_t sub_phase = phase%255;

    switch(group){
        case 0:
            red = 255 - sub_phase;
            green = sub_phase;
            blue = 0;
            break;
        case 1:
            red = 0;
            green = 255 - sub_phase;
            blue = sub_phase;
            break;
        case 2:
            red = sub_phase;
            green = 0;
            blue = 255 - sub_phase;
            break;
        default:
            red = 255;
            green = 0;
            blue = 0;
            break;
    }
    aRGB_value = 0xff000000;
    aRGB_value |= red << 16;
    aRGB_value |= green << 8;
    aRGB_value |= blue;

    aRGB_led_show(aRGB_value);

    lastTick = HAL_GetTick();
    lastPhase = lastPhase + interval;
}

uint8_t buzzerWorkingFlag;


float bsp_BuzzerOn(float _freq,float _targetVolPct){

#define BUZZER_CLOCK_FREQUENCY 84000000
#define BUZZER_CLOCK htim4
#define BUZZER_CLOCK_CHANNEL TIM_CHANNEL_3
#define REFERENCE_MAX_VOL_CCR 5000

    uint16_t arr,cpr;
    arr = (uint16_t)((BUZZER_CLOCK_FREQUENCY/ (float)(BUZZER_CLOCK.Instance->PSC+1))/_freq);
    //cpr = (uint16_t)(duty*(float)arr);
    cpr = (uint16_t)(_targetVolPct*REFERENCE_MAX_VOL_CCR*0.5);
    if(cpr > arr/2){
        cpr = arr/2;
    }


    if(!buzzerWorkingFlag){
        buzzerWorkingFlag = 1;
        HAL_TIM_Base_Start(&BUZZER_CLOCK);
        HAL_TIM_PWM_Start(&BUZZER_CLOCK,BUZZER_CLOCK_CHANNEL);
    }
    BUZZER_CLOCK.Instance->ARR = arr;
            __HAL_TIM_SetCompare(&BUZZER_CLOCK,BUZZER_CLOCK_CHANNEL,cpr);

    return (float)cpr/(float)arr*2;
}

void bsp_BuzzerOff(){
    if(buzzerWorkingFlag){
        buzzerWorkingFlag = 0;
        HAL_TIM_PWM_Stop(&BUZZER_CLOCK,BUZZER_CLOCK_CHANNEL);
    }
}

volatile float vccMoni = 0;
volatile float vccBat = 0;

void bsp_ADC_vccMoni(){
    static uint8_t calibrationCplt = 0;
    static uint32_t calibrationCnt = 0;
    static uint32_t vrefSum = 0,vrefValue = 0;
    if(calibrationCplt == 0){
        if (calibrationCnt == 0){
            calibrationCnt ++;
            HAL_ADC_Start(&hadc1);
            HAL_ADC_PollForConversion(&hadc1, 2);
            vrefValue = HAL_ADC_GetValue(&hadc1);
            vrefSum += vrefValue;
        }else if(calibrationCnt < 200){
            calibrationCnt ++;
            HAL_ADC_Start(&hadc1);
            HAL_ADC_PollForConversion(&hadc1, 2);
            vrefValue = HAL_ADC_GetValue(&hadc1);
            vrefSum += vrefValue;
        }else{
            calibrationCplt = 1;
            vrefValue = vrefSum/200.0f;
            HAL_ADC_Stop(&hadc1);

            HAL_ADC_Start(&hadc3);
            HAL_ADC_PollForConversion(&hadc3, 2);
            vccMoni = HAL_ADC_GetValue(&hadc3)*1.25f/(float)vrefValue;

        }

    }else{
        HAL_ADC_Start(&hadc3);
        HAL_ADC_PollForConversion(&hadc3, 2);
        vccMoni = HAL_ADC_GetValue(&hadc3)*1.25f/(float)vrefValue;

    }
    // ⚠️ 2026-10-17：这个换算(/22*222)看着像笔误，从没和万用表核对过。
    // 用它做保护之前，必须先打印 vccBat 跟实测电压对比，反推出正确系数。
    vccBat = vccMoni/22.0f*222.0f;
}


#define FLASH_SECTOR_9_ADDRESS 0x080A0000
flash_data_t flashData;
void bsp_flash_write(flash_data_t *_flashData){
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef eraseInit;
    eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
    eraseInit.Sector = FLASH_SECTOR_9;
    eraseInit.NbSectors = 1;
    eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    HAL_FLASHEx_Erase(&eraseInit,NULL);
    uint32_t flash_ptr = FLASH_SECTOR_9_ADDRESS;

    for (uint32_t *ptr = (uint32_t*)_flashData;ptr < (uint32_t*)(sizeof(flash_data_t)/4 + _flashData);ptr++){
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,flash_ptr,*ptr);
        flash_ptr += 4;
    }
    HAL_FLASH_Lock();
}
void bsp_flash_read(flash_data_t *_flashData){
    uint32_t flash_ptr = FLASH_SECTOR_9_ADDRESS;
    memcpy(_flashData,(uint32_t*)FLASH_SECTOR_9_ADDRESS,sizeof(flash_data_t)/4);
}



uint32_t init_Flag = 0;
uint32_t period = 5000;

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if(init_Flag == 0)
        return;
    if(htim == &htim10){
        // 1ms loop
        aRGB_led_change(period);
        bsp_ADC_vccMoni();

        static uint32_t cnt = 0;
        cnt++;

        CtrlHandle(); // 全车控制
        ChassisHandle(); // 底盘解算
        UserHandle(); // 用户自定义任务
        Motor::CANPackageSend(); // 给电机发包，进行控制

        if(cnt>20){
            // ===== 本地修改 2026-10-17：看门狗喂法 =====
            // 看门狗只负责抓"程序卡死"：只要这个 1ms 中断还在跑，就无条件喂。
            // 原代码是 if(vccBat<10) 才喂——那是把"电压检测"塞进了喂狗条件：
            //   没电池(vccBat≈0) → 一直喂 → 看着正常；
            //   接上 24V 电池(vccBat≥10) → 没人喂 → 每 1.02s 复位一次。
            // 教训：喂狗条件必须只跟"程序是否活着"有关，不能带任何业务判断。
            // 注意：init 阶段如果新增了 >1s 的等待（比如 IMU 标定），记得在循环里喂狗。
            HAL_IWDG_Refresh(&hiwdg);

            // 低压保护放这里（不是放喂狗条件里）。阈值/系数要实测：
            // vccBat = vccMoni/22.0f*222.0f 这个换算疑似笔误，先打印再谈保护。
            // if (vccBat < 18.0f) { /* TODO: 限流 / 停机 / 只报警 */ }
            cnt = 0;
        }
    }
}

volatile uint8_t key_raw_state = 1;//
uint32_t key_last_stamp;


void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin){
    if(init_Flag == 0)return;
    if (GPIO_Pin == GPIO_PIN_0){
        key_raw_state = HAL_GPIO_ReadPin(KEY_GPIO_Port,KEY_Pin);
        uint32_t time_interval = HAL_GetTick() - key_last_stamp;
        key_last_stamp = HAL_GetTick();
        if (time_interval >= 50){
            if (key_raw_state == 1){
                period = 5000;
            }else{
                period = 500;
            }
        }
    }
}

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */


    /* MCU Configuration--------------------------------------------------------*/

    /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
    HAL_Init();

    /* USER CODE BEGIN Init */
    // 2026-10-21：把 SysTick 优先级提到最高（0）。
    // 原因：所有 HAL 函数的超时都是拿 HAL_GetTick() 差值判的。原代码里 SysTick 优先级最低，
    // 一旦在 TIM10 的 1ms 中断里调用 HAL_SPI_TransmitReceive（超时 100ms），SysTick 无法抢占
    // → tick 不涨 → "超时"永不触发 → 死循环 → 整个 1ms 中断回不来 → USB 控制传输全废
    // （现象：设备能枚举，但 COM 口打不开，报 参数错误 87）。
    // 把 SysTick 提到 0 优先级后，所有 HAL 超时才真正有效。
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_TIM5_Init();
    MX_TIM1_Init();
    MX_TIM4_Init();
    MX_TIM10_Init();
    MX_TIM8_Init();
    MX_ADC1_Init();
    MX_ADC3_Init();
    MX_USART1_UART_Init();
    MX_USART6_UART_Init();
    MX_USART3_UART_Init();
    MX_CAN1_Init();
    MX_CAN2_Init();
    MX_I2C3_Init();
    MX_SPI1_Init();
    MX_SPI2_Init();
    MX_IWDG_Init();
    MX_USB_DEVICE_Init();
    /* USER CODE BEGIN 2 */

    HAL_TIM_Base_Start_IT(&htim5);

    HAL_TIM_PWM_Start(&htim5,TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim5,TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim5,TIM_CHANNEL_3);

    RemoteControl::init();
    bsp_flash_read(&flashData);
    HAL_TIM_Base_Start_IT(&htim10);
    Motor::Init();
    ChassisStart();
    UserInit();
    init_Flag = 1;
    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */

        /* USER CODE BEGIN 3 */
        UserPollSlow();   // 2026-10-21：IMU 的 SPI 读放主循环，绝不占 1ms 中断
    }
    /* USER CODE END 3 */
}