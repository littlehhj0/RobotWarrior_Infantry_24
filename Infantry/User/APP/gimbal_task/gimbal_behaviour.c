/**
  ********************************RM Warrior 2023*******************************
  * @file       gimbal_task.c/h
  * @brief      完成云台控制任务，由于云台使用陀螺仪解算出的角度，其范围在（-pi,pi）
  *             故而设置目标角度均为范围，存在许多对角度计算的函数。云台主要分为2种
  *             状态，陀螺仪控制状态是利用板载陀螺仪解算的姿态角进行控制，编码器控制
  *             状态是通过电机反馈的编码值控制的校准，此外还有校准状态，停止状态等。
  * @note       
  * @history
  *  Version    Date            Author          Modification
  *  V1.0.0     2023/2/         pxx              ......
  *
  @verbatim
  ==============================================================================

  ==============================================================================
  @endverbatim
  ********************************RM Warrior 2023*******************************
  */

#include "gimbal_behaviour.h"
#include "chassis_behaviour.h"
#include "arm_math.h"
#include "buzzer.h"
// #include "Detect_Task.h"

#include "led.h"
#include "Laser.h"

#include "user_lib.h"
#include "INS_task.h"
#include "Kalman_Filter.h"
#include "stdio.h"
////云台校准蜂鸣器响声
//#define GIMBALWarnBuzzerOn() buzzer_on(31, 20000)
//#define GIMBALWarnBuzzerOFF() buzzer_off()

#define int_abs(x) ((x) > 0 ? (x) : (-x))
uint8_t init_step = 3;// 第一次上电，3:1减速比找中值
KalmanInfo accel_x_kalman;
KalmanInfo accel_y_kalman; 
/**
  * @brief          遥控器的死区判断，因为遥控器的拨杆在中位的时候，不一定是发送1024过来，
  * @author         RM
  * @param[in]      输入的遥控器值
  * @param[in]      输出的死区处理后遥控器值
  * @param[in]      死区值
  * @retval         返回空
  */
#define rc_deadline_limit(input, output, dealine)        \
    {                                                    \
        if ((input) > (dealine) || (input) < -(dealine)) \
        {                                                \
            (output) = (input);                          \
        }                                                \
        else                                             \
        {                                                \
            (output) = 0;                                \
        }                                                \
    }

/**
  * @brief          云台校准的通过判断角速度来判断云台是否到达极限位置
  * @author         RM
  * @param[in]      对应轴的角速度，单位rad/s
  * @param[in]      计时时间，到达GIMBAL_CALI_STEP_TIME的时间后归零
  * @param[in]      记录的角度 rad
  * @param[in]      反馈的角度 rad
  * @param[in]      记录的编码值 raw
  * @param[in]      反馈的编码值 raw
  * @param[in]      校准的步骤 完成一次 加一
  * @retval         返回空
  */
#define GIMBAL_CALI_GYRO_JUDGE(gyro, cmd_time, angle_set, angle, ecd_set, ecd, step) \
    {                                                                                \
        if ((gyro) < GIMBAL_CALI_GYRO_LIMIT)                                         \
        {                                                                            \
            (cmd_time)++;                                                            \
            if ((cmd_time) > GIMBAL_CALI_STEP_TIME)                                  \
            {                                                                        \
                (cmd_time) = 0;                                                      \
                (angle_set) = (angle);                                               \
                (ecd_set) = (ecd);                                                   \
                (step)++;                                                            \
            }                                                                        \
        }                                                                            \
    }

/**
  * @brief          云台行为状态机设置，因为在cali等模式下使用了return，故而再用了一个函数
  * @author         RM
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_behavour_set(Gimbal_Control_t *gimbal_mode_set);

/**
  * @brief          云台无力控制，在这个模式下发送的yaw，pitch 是电机控制原始值，云台电机发送can零控制量，使得云台无力
  * @author         RM
  * @param[in]      发送yaw电机的原始值，会直接通过can 发送到电机
  * @param[in]      发送pitch电机的原始值，会直接通过can 发送到电机
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_zero_force_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);
/**
  * @brief          云台初始化控制，电机是陀螺仪角度控制，云台先抬起pitch轴，后旋转yaw轴
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_init_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);

/**
  * @brief          云台校准控制，电机是raw控制，云台先抬起pitch，放下pitch，在正转yaw，最后反转yaw，记录当时的角度和编码值
  * @author         RM
  * @param[in]      发送yaw电机的原始值，会直接通过can 发送到电机
  * @param[in]      发送pitch电机的原始值，会直接通过can 发送到电机
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_cali_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);
/**
  * @brief          云台陀螺仪控制，电机是陀螺仪角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_absolute_angle_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);
/**
  * @brief          云台编码值控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_relative_angle_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);
/**
  * @brief          云台进入遥控器无输入控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_motionless_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);

/**
  * @brief          云台进入自瞄控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_autoshoot_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set);

/**
 * @brief 获取云台行为状态机
 * 
 * @return gimbal_behaviour_e 
 */
gimbal_behaviour_e get_gimbal_behaviour(void);

//云台行为状态机
static gimbal_behaviour_e gimbal_behaviour = GIMBAL_ZERO_FORCE;
static gimbal_behaviour_e last_gimbal_behaviour = GIMBAL_ZERO_FORCE;

/**
  * @brief          云台行为状态机以及电机状态机设置
  * @author         RM
  * @param[in]      云台数据指针
  * @retval         返回空
  */

//gimbal_motor_mode_e test = GIMBAL_MOTOR_ENCONDE;
void gimbal_behaviour_mode_set(Gimbal_Control_t *gimbal_mode_set)
{
    if (gimbal_mode_set == NULL)
    {
        return;
    }
    //云台行为状态机设置
    gimbal_behavour_set(gimbal_mode_set);
    // gimbal_behaviour = GIMBAL_AUTO_SHOOT;

    // 自瞄切换状态平稳过渡
    if(gimbal_behaviour == GIMBAL_AUTO_SHOOT && last_gimbal_behaviour != GIMBAL_AUTO_SHOOT)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_cmd_slow_set.out = gimbal_mode_set->gimbal_yaw_motor.absolute_angle;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_cmd_slow_set.out = gimbal_mode_set->gimbal_pitch_motor.absolute_angle;
    }
    last_gimbal_behaviour = gimbal_behaviour;

    //根据云台行为状态机设置电机状态机
    if (gimbal_behaviour == GIMBAL_ZERO_FORCE)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_RAW;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_RAW;
    }
    else if (gimbal_behaviour == GIMBAL_INIT)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
    }
    else if (gimbal_behaviour == GIMBAL_CALI)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_RAW;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_RAW;
    }
    else if (gimbal_behaviour == GIMBAL_ABSOLUTE_ANGLE)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
    }
    else if (gimbal_behaviour == GIMBAL_RELATIVE_ANGLE)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
    }
    else if (gimbal_behaviour == GIMBAL_MOTIONLESS)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_ENCONDE;
    }
    else if (gimbal_behaviour == GIMBAL_AUTO_SHOOT)
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
    }

    if(rotation_cmd_gimbal_absolute())
    {
        gimbal_mode_set->gimbal_yaw_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
        gimbal_mode_set->gimbal_pitch_motor.gimbal_motor_mode = GIMBAL_MOTOR_GYRO;
    }

    //手动设置
    if(gimbal_behaviour == GIMBAL_ABSOLUTE_ANGLE && (gimbal_mode_set->gimbal_rc_ctrl->key.v & TestKeyBoard))
    {
        gimbal_mode_set->ecd_count = (gimbal_mode_set->ecd_count + 1) % 3;
    }


    gimbal_mode_set->last_super_channel = gimbal_mode_set->gimbal_rc_ctrl->rc.s[SUPER_MODE_CHANNEL];
}

/**
  * @brief          云台行为控制，根据不同行为采用不同控制函数
  * @author         RM
  * @param[in]      设置的yaw角度增加值，单位 rad
  * @param[in]      设置的pitch角度增加值，单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
void gimbal_behaviour_control_set(fp32 *add_yaw, fp32 *add_pitch, Gimbal_Control_t *gimbal_control_set)
{

    if (add_yaw == NULL || add_pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }

    static fp32 rc_add_yaw, rc_add_pit;
    static int16_t yaw_channel = 0, pitch_channel = 0;

    //将遥控器的数据处理死区 int16_t yaw_channel,pitch_channel
    rc_deadline_limit(gimbal_control_set->gimbal_rc_ctrl->rc.ch[YawChannel], yaw_channel, RC_deadband);
    rc_deadline_limit(gimbal_control_set->gimbal_rc_ctrl->rc.ch[PitchChannel], pitch_channel, RC_deadband);

    rc_add_yaw = yaw_channel * Yaw_RC_SEN - gimbal_control_set->gimbal_rc_ctrl->mouse.x * Yaw_Mouse_Sen;
    rc_add_pit = pitch_channel * Pitch_RC_SEN - gimbal_control_set->gimbal_rc_ctrl->mouse.y * Pitch_Mouse_Sen;

    if (gimbal_behaviour == GIMBAL_ZERO_FORCE)
    {
        gimbal_zero_force_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_INIT)
    {
        gimbal_init_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_CALI)
    {
        gimbal_cali_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_ABSOLUTE_ANGLE)
    {
        gimbal_absolute_angle_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_RELATIVE_ANGLE)
    {
        gimbal_relative_angle_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_MOTIONLESS)
    {
        gimbal_motionless_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    else if (gimbal_behaviour == GIMBAL_AUTO_SHOOT)
    {
        gimbal_autoshoot_control(&rc_add_yaw, &rc_add_pit, gimbal_control_set);
    }
    //将控制增加量赋值
    *add_yaw = rc_add_yaw;
    *add_pitch = rc_add_pit;
}

/**
  * @brief          云台在某些行为下，需要底盘不动
  * @author         RM
  * @param[in]      void
  * @retval         返回空
  */

bool_t gimbal_cmd_to_chassis_stop(void)
{
    if (gimbal_behaviour == GIMBAL_INIT || gimbal_behaviour == GIMBAL_CALI || gimbal_behaviour == GIMBAL_MOTIONLESS || gimbal_behaviour == GIMBAL_ZERO_FORCE)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}
/**
  * @brief          云台在初始化行为下，需要底盘短时间移动
  * @author         RM
  * @param[in]      void
  * @retval         返回空
  */

uint8_t gimbal_init_cmd_chassis_move(void)
{
    if (init_step == 1)
    {
        return 1;
    }
    else if(init_step == 2)
    {
        return 2;
    }
    else if (init_step == 0 || init_step == 3)
    {
        return 3;
    }
    else
    {
        return 0;
    }
}


/**
  * @brief          云台在某些行为下，需要射击停止
  * @author         RM
  * @param[in]      void
  * @retval         返回空
  */
bool_t gimbal_cmd_to_shoot_stop(void)
{
    if (gimbal_behaviour == GIMBAL_INIT || gimbal_behaviour == GIMBAL_CALI || gimbal_behaviour == GIMBAL_ZERO_FORCE)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

/**
  * @brief          云台在无力时，蜂鸣器提醒电源电量
  * @param[in]      void
  * @retval         1：云台无力
  */

bool_t gimbal_cmd_to_voltage_warning_stop(void)
{
    if(gimbal_behaviour == GIMBAL_ZERO_FORCE)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}


/**
  * @brief          云台行为状态机设置，因为在cali等模式下使用了return，故而再用了一个函数
  * @author         RM
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_behavour_set(Gimbal_Control_t *gimbal_mode_set)
{
    static uint8_t countsuper = 0;//超级模式状态计数器

    if (gimbal_mode_set == NULL)
    {
        return;
    }
    //校准行为，return 不会设置其他的模式 
    if (gimbal_behaviour == GIMBAL_CALI && gimbal_mode_set->gimbal_cali.step != GIMBAL_CALI_END_STEP)
    {
        return;
    }
    //如果外部使得校准步骤从0 变成 start，则进入校准模式
    if (gimbal_mode_set->gimbal_cali.step == GIMBAL_CALI_START_STEP)
    {
        gimbal_behaviour = GIMBAL_CALI;
        return;
    }

    //初始化模式判断是否到达中值位置
    //若init_step为0，步兵向前冲并记录加速度，得出云台校准位置，若init_step为3，不冲刺
    if (gimbal_behaviour == GIMBAL_INIT)
    {
        if(init_step == 0 || init_step == 3)
        {
            static uint16_t init_time = 0;
            static uint16_t init_stop_time = 0;
            init_time++;
            //到达中值 计时
            if ((fabs(gimbal_mode_set->gimbal_yaw_motor.relative_angle - INIT_YAW_SET) < GIMBAL_INIT_ANGLE_ERROR &&
                fabs(gimbal_mode_set->gimbal_pitch_motor.absolute_angle - INIT_PITCH_SET) < GIMBAL_INIT_ANGLE_ERROR))
            {
                //到达初始化位置
                if (init_stop_time < GIMBAL_INIT_STOP_TIME)
                {
                    init_stop_time++;
                }
            }
            else
            {
                //没有到达初始化位置，时间计时
                if (init_time < GIMBAL_INIT_TIME)
                {
                    init_time++;
                }
            }

            //超过初始化最大时间，或者已经稳定到中值一段时间，退出初始化状态开关打下档，或者掉线
            if (init_time < GIMBAL_INIT_TIME && init_stop_time < GIMBAL_INIT_STOP_TIME &&
                !switch_is_down(gimbal_mode_set->gimbal_rc_ctrl->rc.s[ModeChannel]) )//&& !toe_is_error(DBUSTOE))
            {
                return;
            }
            else
            {
                buzzer_off();
                init_stop_time = 0;
                init_time = 0;
                if(init_step == 0)
                {
                    init_step = 1;
                    Kalman_Filter_Init(&accel_x_kalman);
                    Kalman_Filter_Init(&accel_y_kalman);
                    return;
                }
                else if(init_step == 3)
                {
                    
                }
            }
        }
        else if(init_step == 1 || init_step == 2)
        {
            static int16_t yaw_ecd_cali_time = 0;
            yaw_ecd_cali_time++;
            if(init_step == 1)
            {
                if(yaw_ecd_cali_time > 70)
                {
                    yaw_ecd_cali_time = 0;
                    init_step = 2;
                }
                return;
            }
            else if(init_step == 2)
            {
                const fp32 *local_accel;
                fp32 vx = 0.0f,vy = 0.0f;
                local_accel = get_accel_filter_point();
                Kalman_Filter_Fun(&accel_x_kalman,local_accel[0]);
                Kalman_Filter_Fun(&accel_y_kalman,local_accel[1]);
                if(fabs(accel_x_kalman.out) > 0.8f)
                    vx += accel_x_kalman.out;
                if(fabs(accel_y_kalman.out) > 0.8f)
                    vy += accel_y_kalman.out;
                printf("%.2f,%.2f\r\n",accel_x_kalman.out,accel_y_kalman.out);

                // if(fabs(accel_x_kalman.out) > 0.8f)
                //     vx += local_accel[0];
                // if(fabs(accel_y_kalman.out) > 0.8f)
                //     vy += local_accel[1];
                // printf("%.2f,%.2f\r\n",local_accel[0],local_accel[1]);

                if(yaw_ecd_cali_time > 100)
                {
                    //vx,vy判断时要加绝对值
                    //多次测试发现，ecd=2时，k在负一左右，ecd=1时，k在1左右，适用于2024.3新版步兵
                    if(fabs(vx) < 0.5f && fabs(vy) < 0.5f)
                    {
                        gimbal_mode_set->ecd_count = 0;
                        // printf("第一种vx==%.2f,vy==%.2f,ecd_count==%f\n",vx,vy,gimbal_mode_set->ecd_count);
                    }
                    else
                    {
                        fp32 k = vy / vx;
                        if(k > -0.4f && k < 0.8f) 
                        {
                            gimbal_mode_set->ecd_count = 0;
                        }
                        else if(k >= 0.8f)
                        {
                            gimbal_mode_set->ecd_count = 2;
                        }
                        else
                        {
                            gimbal_mode_set->ecd_count = 1;
                        }
                        // printf("第二种vx==%.2f,vy==%.2f,k==%.2f,ecd_count==%d\n",vx,vy,k,gimbal_mode_set->ecd_count);
                    }
                    init_step = 3;
                }
            }
            return;
        }
    }

    //开关控制 云台状态
    if (switch_is_down(gimbal_mode_set->gimbal_rc_ctrl->rc.s[ModeChannel]))
    {
        gimbal_behaviour = GIMBAL_ZERO_FORCE;
        gimbal_mode_set->last_super_channel = gimbal_mode_set->gimbal_rc_ctrl->rc.s[SUPER_MODE_CHANNEL];
        countsuper = 0;
    }
    else if (switch_is_mid(gimbal_mode_set->gimbal_rc_ctrl->rc.s[ModeChannel]))
    {
        gimbal_behaviour = GIMBAL_RELATIVE_ANGLE;
    }
    else if (switch_is_up(gimbal_mode_set->gimbal_rc_ctrl->rc.s[ModeChannel]))
    {
        gimbal_behaviour = GIMBAL_ABSOLUTE_ANGLE;
    }

    //超级模式判断进入
    {
        if(switch_is_up(gimbal_mode_set->gimbal_rc_ctrl->rc.s[SUPER_MODE_CHANNEL]) && switch_is_mid(gimbal_mode_set->last_super_channel) 
            && countsuper == 0 && !switch_is_down(gimbal_mode_set->gimbal_rc_ctrl->rc.s[ModeChannel]))
        {
            countsuper += 1;
        }
        else if (switch_is_up(gimbal_mode_set->gimbal_rc_ctrl->rc.s[SUPER_MODE_CHANNEL]) && switch_is_mid(gimbal_mode_set->last_super_channel) && countsuper == 1)
        {
            countsuper -= 1;
        }
        
        // //底盘小陀螺
        // if(rotation_cmd_gimbal_absolute() && countsuper < 2)
        // {
        //     countsuper += 2;
        // }
        // else if(countsuper >= 2)
        // {
        //     countsuper -= 2;
        // }
        if(countsuper == 0)
        {
            led_red_off();
        }
        else if(countsuper == 1)
        {
            led_red_on();
            gimbal_behaviour = GIMBAL_AUTO_SHOOT;
            return;
        }
    }

    // if( toe_is_error(DBUSTOE))
    // {

    //     gimbal_behaviour = GIMBAL_ZERO_FORCE;
    // }

    //判断进入init状态机
    {
        static gimbal_behaviour_e last_gimbal_behaviour = GIMBAL_ZERO_FORCE;
        if (last_gimbal_behaviour == GIMBAL_ZERO_FORCE && gimbal_behaviour != GIMBAL_ZERO_FORCE)
        {
            gimbal_behaviour = GIMBAL_INIT;
            buzzer_on(84, 10);
        }
        last_gimbal_behaviour = gimbal_behaviour;
    }

    //判断进入motionless状态机
    static uint16_t motionless_time = 0;
    if (gimbal_behaviour == GIMBAL_ABSOLUTE_ANGLE && !rotation_cmd_gimbal_absolute())
    {
        //遥控器 键盘均无输入，进入motionless状态
        if (int_abs(gimbal_mode_set->gimbal_rc_ctrl->rc.ch[0]) < GIMBAL_MOTIONLESS_RC_DEADLINE && int_abs(gimbal_mode_set->gimbal_rc_ctrl->rc.ch[1]) < GIMBAL_MOTIONLESS_RC_DEADLINE && int_abs(gimbal_mode_set->gimbal_rc_ctrl->rc.ch[2]) < GIMBAL_MOTIONLESS_RC_DEADLINE && int_abs(gimbal_mode_set->gimbal_rc_ctrl->rc.ch[3]) < GIMBAL_MOTIONLESS_RC_DEADLINE && int_abs(gimbal_mode_set->gimbal_rc_ctrl->mouse.x) < GIMBAL_MOTIONLESS_RC_DEADLINE && int_abs(gimbal_mode_set->gimbal_rc_ctrl->mouse.y) < GIMBAL_MOTIONLESS_RC_DEADLINE && gimbal_mode_set->gimbal_rc_ctrl->key.v == 0 && gimbal_mode_set->gimbal_rc_ctrl->mouse.press_l == 0 && gimbal_mode_set->gimbal_rc_ctrl->mouse.press_r == 0)
        {
            if (motionless_time < GIMBAL_MOTIONLESS_TIME_MAX)
            {
                motionless_time++;
            }
        }
        else
        {
            motionless_time = 0;
        }

        if (motionless_time == GIMBAL_MOTIONLESS_TIME_MAX)
        {
            gimbal_behaviour = GIMBAL_MOTIONLESS;
        }
    }
    else
    {
        motionless_time = 0;
    }


}

/**
  * @brief          云台无力控制，在这个模式下发送的yaw，pitch 是电机控制原始值，云台电机发送can零控制量，使得云台无力
  * @author         RM
  * @param[in]      发送yaw电机的原始值，会直接通过can 发送到电机
  * @param[in]      发送pitch电机的原始值，会直接通过can 发送到电机
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_zero_force_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }

    *yaw = 0.0f;
    *pitch = 0.0f;
}
/**
  * @brief          云台初始化控制，电机是陀螺仪角度控制，云台先抬起pitch轴，后旋转yaw轴
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_init_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }

    //初始化状态控制量计算
    if (fabs(INIT_PITCH_SET - gimbal_control_set->gimbal_pitch_motor.absolute_angle) > GIMBAL_INIT_ANGLE_ERROR)
    {
        *pitch = (INIT_PITCH_SET - gimbal_control_set->gimbal_pitch_motor.absolute_angle) * GIMBAL_INIT_PITCH_SPEED;
        *yaw = 0.0f;
        // printf("%.2f",fabs(INIT_PITCH_SET - gimbal_control_set->gimbal_pitch_motor.absolute_angle));
        // *yaw = (INIT_YAW_SET - gimbal_control_set->gimbal_yaw_motor.relative_angle) * GIMBAL_INIT_YAW_SPEED;
    }
    else
    {
        *pitch = (INIT_PITCH_SET - gimbal_control_set->gimbal_pitch_motor.absolute_angle) * GIMBAL_INIT_PITCH_SPEED;
        *yaw = (INIT_YAW_SET - gimbal_control_set->gimbal_yaw_motor.relative_angle) * GIMBAL_INIT_YAW_SPEED;
    }
}

/**
  * @brief          云台校准控制，电机是raw控制，云台先抬起pitch，放下pitch，在正转yaw，最后反转yaw，记录当时的角度和编码值
  * @author         RM
  * @param[in]      发送yaw电机的原始值，会直接通过can 发送到电机
  * @param[in]      发送pitch电机的原始值，会直接通过can 发送到电机
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_cali_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }
    static uint16_t cali_time = 0;

    if (gimbal_control_set->gimbal_cali.step == GIMBAL_CALI_PITCH_MAX_STEP)
    {

        *pitch = GIMBAL_CALI_MOTOR_SET;
        *yaw = 0;

        //判断陀螺仪数据， 并记录最大最小角度数据
        GIMBAL_CALI_GYRO_JUDGE(gimbal_control_set->gimbal_pitch_motor.motor_gyro, cali_time, gimbal_control_set->gimbal_cali.max_pitch,
                               gimbal_control_set->gimbal_pitch_motor.absolute_angle, gimbal_control_set->gimbal_cali.max_pitch_ecd,
                               gimbal_control_set->gimbal_pitch_motor.gimbal_motor_measure->ecd, gimbal_control_set->gimbal_cali.step);
    }
    else if (gimbal_control_set->gimbal_cali.step == GIMBAL_CALI_PITCH_MIN_STEP)
    {
        *pitch = -GIMBAL_CALI_MOTOR_SET;
        *yaw = 0;

        GIMBAL_CALI_GYRO_JUDGE(gimbal_control_set->gimbal_pitch_motor.motor_gyro, cali_time, gimbal_control_set->gimbal_cali.min_pitch,
                               gimbal_control_set->gimbal_pitch_motor.absolute_angle, gimbal_control_set->gimbal_cali.min_pitch_ecd,
                               gimbal_control_set->gimbal_pitch_motor.gimbal_motor_measure->ecd, gimbal_control_set->gimbal_cali.step);
    }
    else if (gimbal_control_set->gimbal_cali.step == GIMBAL_CALI_YAW_MAX_STEP)
    {
        *pitch = 0;
        *yaw = GIMBAL_CALI_MOTOR_SET;

        GIMBAL_CALI_GYRO_JUDGE(gimbal_control_set->gimbal_yaw_motor.motor_gyro, cali_time, gimbal_control_set->gimbal_cali.max_yaw,
                               gimbal_control_set->gimbal_yaw_motor.absolute_angle, gimbal_control_set->gimbal_cali.max_yaw_ecd,
                               gimbal_control_set->gimbal_yaw_motor.gimbal_motor_measure->ecd, gimbal_control_set->gimbal_cali.step);
    }

    else if (gimbal_control_set->gimbal_cali.step == GIMBAL_CALI_YAW_MIN_STEP)
    {
        *pitch = 0;
        *yaw = -GIMBAL_CALI_MOTOR_SET;

        GIMBAL_CALI_GYRO_JUDGE(gimbal_control_set->gimbal_yaw_motor.motor_gyro, cali_time, gimbal_control_set->gimbal_cali.min_yaw,
                               gimbal_control_set->gimbal_yaw_motor.absolute_angle, gimbal_control_set->gimbal_cali.min_yaw_ecd,
                               gimbal_control_set->gimbal_yaw_motor.gimbal_motor_measure->ecd, gimbal_control_set->gimbal_cali.step);
    }
    else if (gimbal_control_set->gimbal_cali.step == GIMBAL_CALI_END_STEP)
    {
        cali_time = 0;
    }
}
/**
  * @brief          云台陀螺仪控制，电机是陀螺仪角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_absolute_angle_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }

    // {
    //     static uint16_t last_turn_keyboard = 0;
    //     static uint8_t gimbal_turn_flag = 0;
    //     static fp32 gimbal_end_angle = 0.0f;

    //     if ((gimbal_control_set->gimbal_rc_ctrl->key.v & TurnKeyBoard) && !(last_turn_keyboard & TurnKeyBoard))
    //     {
    //         if (gimbal_turn_flag == 0)
    //         {
    //             gimbal_turn_flag = 1;
    //             //保存掉头的目标值
    //             gimbal_end_angle = rad_format(gimbal_control_set->gimbal_yaw_motor.absolute_angle + PI);
    //         }
    //     }
    //     last_turn_keyboard = gimbal_control_set->gimbal_rc_ctrl->key.v ;

    //     if (gimbal_turn_flag)
    //     {
    //         //不断控制到掉头的目标值，正转，反装是随机
    //         if (rad_format(gimbal_end_angle - gimbal_control_set->gimbal_yaw_motor.absolute_angle) > 0.0f)
    //         {
    //             *yaw += TurnSpeed;
    //         }
    //         else
    //         {
    //             *yaw -= TurnSpeed;
    //         }
    //     }
    //     //到达pi （180°）后停止
    //     if (gimbal_turn_flag && fabs(rad_format(gimbal_end_angle - gimbal_control_set->gimbal_yaw_motor.absolute_angle)) < 0.01f)
    //     {
    //         gimbal_turn_flag = 0;
    //     }
    // }
}
/**
  * @brief          云台编码值控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_relative_angle_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }
    
    if(rotation_cmd_gimbal_absolute())
    {
        //不需要处理，
    }
    else
    {
        *yaw = 0;
    }
}
/**
  * @brief          云台进入遥控器无输入控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_motionless_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }
    *yaw = 0.0f;
    *pitch = 0.0f;
}

/**
  * @brief          云台进入自瞄控制，电机是相对角度控制，
  * @author         RM
  * @param[in]      yaw轴角度控制，为角度的增量 单位 rad
  * @param[in]      pitch轴角度控制，为角度的增量 单位 rad
  * @param[in]      云台数据指针
  * @retval         返回空
  */
static void gimbal_autoshoot_control(fp32 *yaw, fp32 *pitch, Gimbal_Control_t *gimbal_control_set)
{
    if (yaw == NULL || pitch == NULL || gimbal_control_set == NULL)
    {
        return;
    }
    *yaw = 0.0f;
    *pitch = 0.0f;

    // fp32 add_pitch = 0.0f, add_yaw = 0.0f;
    // Get_Gimbal_Angle(&add_yaw, &add_pitch);

    // fp32 add_pitch = 0.0f, add_yaw = 0.0f;
    // if(gimbal_control_set->gimbal_ros_msg->shoot_depth != 0)
    // {
    //     add_yaw = gimbal_control_set->gimbal_ros_msg->shoot_yaw - gimbal_control_set->gimbal_yaw_motor.absolute_angle_set;
    //     add_pitch = gimbal_control_set->gimbal_ros_msg->shoot_pitch - gimbal_control_set->gimbal_pitch_motor.absolute_angle_set;
    // }
    // *pitch = add_pitch;
    // *yaw = add_yaw;

    fp32 set_yaw = 0.0f, set_pitch = 0.0f;
    if(gimbal_control_set->gimbal_ros_msg->shoot_depth != 0)
    {
        Laser_On();
        set_yaw = gimbal_control_set->gimbal_ros_msg->shoot_yaw;
        set_pitch = gimbal_control_set->gimbal_ros_msg->shoot_pitch;

        // *yaw = set_yaw - gimbal_control_set->gimbal_yaw_motor.absolute_angle_set;
        // *pitch = set_pitch - gimbal_control_set->gimbal_pitch_motor.absolute_angle_set;

        // 低通滤波
        first_order_filter_cali(&gimbal_control_set->gimbal_yaw_motor.gimbal_cmd_slow_set, set_yaw);
        first_order_filter_cali(&gimbal_control_set->gimbal_pitch_motor.gimbal_cmd_slow_set, set_pitch);
        *yaw = gimbal_control_set->gimbal_yaw_motor.gimbal_cmd_slow_set.out - gimbal_control_set->gimbal_yaw_motor.absolute_angle_set;
        *pitch = gimbal_control_set->gimbal_pitch_motor.gimbal_cmd_slow_set.out - gimbal_control_set->gimbal_pitch_motor.absolute_angle_set;
    }
    else
    {
        Laser_Off();
    }
} 

gimbal_behaviour_e get_gimbal_behaviour(void)
{
    return gimbal_behaviour;
}
