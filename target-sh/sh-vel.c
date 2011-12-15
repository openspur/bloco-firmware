/* --------------------------------------- */
/* main関数プログラム */
/* 初期化とサーボループ,コマンド受信 */
/* ISSのをベースに作成　　 */
/* --------------------------------------- */

#include <sh7040s.h>							// アドレス定義
#include <motor-device.h>
#include <sh-serial.h>
#include <sh-vel.h>

// ./configureによって生成
#include <config.h>

#define GLOBAL_DEFINE							// グローバル変数設定
#include <sh-globals.h>

#include <shvel-param.h>

#include <string.h>

// 全体の初期化
void init( void )
{
	// シリアル通信、カウンタ、PWM出力の初期化
	sci_init( 38400 );

	// コンペアマッチタイマの初期化
	setIntMask( 9 );							// レベル10〜15は割り込み許可
	initCMT(  );

	initCounter(  );							// エンコーダカウンタの初期化
	initPWM(  );								// PWMの初期化
	initAD(  );
}

// サーボループの割り込み設定
void initCMT( void )
{
	CMT.CMSTR.BIT.STR1 = 0;						/* コンペアマッチタイマ1ストップ */
	CMT1.CMCOR = 28636 / 8 - 1;
	CMT1.CMCSR.BIT.CKS = 0;						/* φの設定 */
	CMT1.CMCSR.BIT.CMIE = 1;					/* 割り込み可設定 */
	INTC.IPRG.BIT._CMT1 = 15;					/* 割り込みコントローラの割り込みレベル */
}

// /////////////////// 割り込み関数 /////////////////////
// //////////////　車輪速度制御ループ /////////////////////
#pragma interrupt
int_cmi1(  )
{
	int i;

	/* エンコーダ値入力 */
	cnt_read(  );

	if( servo_level <= SERVO_LEVEL_STOP )
	{											// servo_level 0 (shut down)
		put_pwm( 0, 0 );
		put_pwm( 1, 0 );
		CMT1.CMCSR.BIT.CMF = 0;

	}
	else
	{											// servo_level 1 (counter enable)

		if( param_change == 3 )
		{
			w_ref[0] = w_ref[2];
			w_ref[1] = w_ref[3];
			param_change = 0;
		}

		w_ref_diff[0] = w_ref[0] - w_ref_before[0];
		w_ref_diff[1] = w_ref[1] - w_ref_before[1];
		w_ref_before[0] = w_ref[0];
		w_ref_before[1] = w_ref[1];

		if( servo_level >= SERVO_LEVEL_TORQUE )
		{										// servo_level 2(toque enable)
			if( servo_level >= SERVO_LEVEL_VELOCITY )
			{									// servo_level 3 (speed enable)
				for ( i = 0; i < MOTOR_NUM; i++ )
				{
					/* 積分 */
					int_w[i] += ( w_ref[i] - cnt_dif[i] );
					if( int_w[i] > int_max[i] )
						int_w[i] = int_max[i];
					if( int_w[i] < int_min[i] )
						int_w[i] = int_min[i];

					/* PI制御分 */
					toq_pi[i] = ( w_ref[i] - cnt_dif[i] ) * p_pi_kp[i] + int_w[i] * p_pi_ki[i];
				}

				/* PWSでの相互の影響を考慮したフィードフォワード */
				s_a = ( toq_pi[0] + w_ref_diff[0] );
				s_b = ( toq_pi[1] + w_ref_diff[1] );

				toq[0] = ( s_a * p_A + s_b * p_C + w_ref[0] * p_E ) >> 8;
				toq[1] = ( s_b * p_B + s_a * p_D + w_ref[1] * p_F ) >> 8;
			}
			else
			{									// servo_level 2(toque enable)
				toq[0] = 0;
				toq[1] = 0;
			}
			/* 出力段 */
			for ( i = 0; i < MOTOR_NUM; i++ )
			{
				/* 摩擦補償（線形） */
				if( cnt_dif[i] > 0 )
				{
					toq[i] += ( p_fr_wplus[i] * cnt_dif[i] + p_fr_plus[i] );
				}
				else if( cnt_dif[i] < 0 )
				{
					toq[i] -= ( p_fr_wminus[i] * ( -cnt_dif[i] ) + p_fr_minus[i] );
				}
				else
				{
					toq[i] = toq[i];
				}

				/* トルク補償 */
				toq[i] += p_toq_offset[i];

				/* トルクでクリッピング */
				if( toq[i] >= toq_max[i] )
				{
					toq[i] = toq_max[i];
				}
				if( toq[i] <= toq_min[i] )
				{
					toq[i] = toq_min[i];
				}

				/* トルク→pwm変換 */
				out_pwm[i] = ( toq[i] * p_ki[i] + cnt_dif[i] * p_kv[i] ) / 65536;

				/* PWMでクリッピング */
				if( out_pwm[i] > pwm_max[i] - 1 )
					out_pwm[i] = pwm_max[i] - 1;
				if( out_pwm[i] < pwm_min[i] + 1 )
					out_pwm[i] = pwm_min[i] + 1;
			}

			/* 出力 */
			put_pwm( 0, out_pwm[0] );
			put_pwm( 1, out_pwm[1] );

			pwm_sum[0] += out_pwm[0];
			pwm_sum[1] += out_pwm[1];
		}										// servo_level 2
		
		cnt_updated++;
		if( cnt_updated == 5 )
		{
			counter_buf[0] = counter[0];
			counter_buf[1] = counter[1];
			pwm_buf[0] = pwm_sum[0];
			pwm_buf[1] = pwm_sum[1];
			pwm_sum[0] = 0;
			pwm_sum[1] = 0;
		}
	
	}											// servo_level 1
	watch_dog++;

	CMT1.CMCSR.BIT.CMF = 0;						// コンペアマッチフラッグのクリア	
}

#define sci_send_txt(c,a) sci_send(c, a,strlen(a))

// //////////////////////////////////////////////////
/* 受信したYPSpur拡張コマンドの解析 */
int extended_command_analyze( int channel, char *data )
{
	char line[64];
	int i;

	if( servo_level != SERVO_LEVEL_STOP )
		return 0;
	if( strstr( data, "VV" ) == data )
	{
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\nVEND:" );
		sci_send_txt( channel, YP_VENDOR_NAME );
		sci_send_txt( channel, "; \nPROD:" );
		sci_send_txt( channel, YP_PRODUCT_NAME );
		sci_send_txt( channel, "; \nFIRM:" );
		sci_send_txt( channel, YP_FIRMWARE_NAME );
		sci_send_txt( channel, "; \nPROT:" );
		sci_send_txt( channel, YP_PROTOCOL_NAME );
		sci_send_txt( channel, "; \nSERI:Reserved; \n\n" );
		// set long timeout
		watch_dog = -1000;
	}
	else if( strstr( data, "ADMASK" ) == data )
	{
		unsigned char tmp;

		tmp = 0;
		for ( i = 6; data[i] != 0 && data[i] != '\n' && data[i] != '\r'; i++ )
		{
			tmp = tmp << 1;
			if( data[i] == '1' )
				tmp |= 1;
		}
		analog_mask = tmp;
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\n\n" );

		// タイムアウトを長めに設定
		watch_dog = -1000;
	}
	else if( strstr( data, "SETIODIR" ) == data )
	{
		unsigned char tmp;
		// PE0-3(0-3), PB2-5(4-7)

		tmp = 0;
		for ( i = 8; data[i] != 0 && data[i] != '\n' && data[i] != '\r'; i++ )
		{
			tmp = tmp << 1;
			if( data[i] == '1' )
				tmp |= 1;
		}
		PFC.PEIOR.WORD  = ( PFC.PEIOR.WORD  & 0xFFF0 ) | ( ( tmp & 0x0F ) << 0 );
		PFC.PBIOR.WORD  = ( PFC.PBIOR.WORD  & 0xFFC3 ) | ( ( tmp & 0xF0 ) >> 2 );
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\n\n" );

		// タイムアウトを長めに設定
		watch_dog = -1000;
	}
	else if( strstr( data, "GETIOVAL" ) == data )
	{
		unsigned short tmp;
		char num[3];
		tmp = ( PE.DR.WORD & 0x0F ) | ( ( PB.DR.WORD  & 0x3C ) << 2 );
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n" );
		if( ( tmp >> 4 ) > 9 ){
			num[0] = ( tmp >> 4 ) - 10 + 'A';
		}else{
			num[0] = ( tmp >> 4 ) + '0';
		}
		if( ( tmp & 0xF ) > 9 ){
			num[1] = ( tmp & 0xF ) - 10 + 'A';
		}else{
			num[1] = ( tmp & 0xF ) + '0';
		}
		num[2] = 0;
		sci_send_txt( channel, num );
		sci_send_txt( channel, " \n\n" );

		// タイムアウトを長めに設定
		watch_dog = -1000;
	}
	else if( strstr( data, "GETIO" ) == data )
	{
		if( data[5] == '1' )
		{
			dio_enable = 1;
		}
		else
		{
			dio_enable = 0;
		}
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\n\n" );

		// タイムアウトを長めに設定
		watch_dog = -1000;
	}
	else if( strstr( data, "OUTPUT" ) == data )
	{
		unsigned char tmp;
		// PA18-21(0-3), PB2-5(4-7)

		tmp = 0;
		for ( i = 6; data[i] != 0 && data[i] != '\n' && data[i] != '\r'; i++ )
		{
			tmp = tmp << 1;
			if( data[i] == '1' )
				tmp |= 1;
		}
		PE.DR.WORD   = ( PE.DR.WORD   & 0xFFF0 ) | ( ( tmp & 0x0F ) << 0 );
		PB.DR.WORD   = ( PB.DR.WORD   & 0xFFC3 ) | ( ( tmp & 0xF0 ) >> 2 );
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\n\n" );

		// タイムアウトを長めに設定
		watch_dog = -1000;
	}
	else if( strstr( data, "SS" ) == data )
	{
		int tmp;
		volatile int lo;

		cnt_updated = 0;
		tmp = 0;
		for ( i = 2; data[i] != 0 && data[i] != '\n' && data[i] != '\r'; i++ )
		{
			tmp *= 10;
			tmp += data[i] - '0';
		}
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n00P\n\n" );
		// 送信終了まで待機
		while( SCI_send_rp[channel] != SCI_send_wp[channel] );
		for ( lo = 0; lo < 10000; lo++ );		/* wait more than 1bit time */
		sci_init( tmp );
		sci_start(  );							// start SCI
		cnt_updated = 0;

		// タイムアウトを長めに設定
		watch_dog = -1000;
		return -1;
	}
	else
	{
		if( data[0] == 0 || data[0] == '\n' || data[0] == '\r' )
			return 0;
		sci_send_txt( channel, data );
		sci_send_txt( channel, "\n0Ee\n\n" );
	}
	return 1;
}

// //////////////////////////////////////////////////
/* 受信したコマンドの解析 */
int command_analyze( char *data, int len )
{
	int motor;

	Int_4Char i;

	i.byte[0] = data[2];
	i.byte[1] = data[3];
	i.byte[2] = data[4];
	i.byte[3] = data[5];

	motor = data[1];
	if( motor < 0 || motor >= 2 )
		return 0;

	// if(data[0] != PARAM_w_ref)
	// SCI1_printf("get %d %d %d\n",data[0],data[1],i.integer);
	switch ( data[0] )
	{
	case PARAM_w_ref:
		w_ref[motor + 2] = i.integer;
		param_change |= ( 1 << motor );
		watch_dog = 0;
		break;
	case PARAM_p_ki:
		p_ki[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_kv:
		p_kv[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_fr_plus:
		p_fr_plus[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_fr_wplus:
		p_fr_wplus[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_fr_minus:
		p_fr_minus[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_fr_wminus:
		p_fr_wminus[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_A:
		p_A = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_B:
		p_B = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_C:
		p_C = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_D:
		p_D = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_E:
		p_E = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_F:
		p_F = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_pi_kp:
		p_pi_kp[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_pi_ki:
		p_pi_ki[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_pwm_max:
		pwm_max[motor] = i.integer;
		if( motor == 0 )
			MTU3.TGRA = i.integer;
		if( motor == 1 )
			MTU3.TGRC = i.integer;
		watch_dog = 0;
		break;
	case PARAM_pwm_min:
		pwm_min[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_toq_max:
		toq_max[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_toq_min:
		toq_min[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_p_toq_offset:
		p_toq_offset[motor] = i.integer;
		watch_dog = 0;
		watch_dog = 0;
		break;
	case PARAM_int_max:
		int_max[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_int_min:
		int_min[motor] = i.integer;
		watch_dog = 0;
		break;
	case PARAM_servo:

		if( servo_level < SERVO_LEVEL_VELOCITY && i.integer >= SERVO_LEVEL_VELOCITY )
		{										// servo levelが速度制御に推移した
			int_w[0] = 0;
			int_w[1] = 0;
		}
		servo_level = i.integer;

		// SCI1_printf("start\n");
		watch_dog = 0;
		break;
	case PARAM_watch_dog_limit:
		p_watch_dog_limit = i.integer;
		watch_dog = 0;
		break;
	case PARAM_io_dir:
		PFC.PEIOR.WORD  = ( PFC.PEIOR.WORD  & 0xFFF0 ) | ( ( i.integer & 0x0F ) << 0 );
		PFC.PBIOR.WORD  = ( PFC.PBIOR.WORD  & 0xFFC3 ) | ( ( i.integer & 0xF0 ) >> 2 );
		watch_dog = 0;
		break;
	case PARAM_io_data:
		PE.DR.WORD   = ( PE.DR.WORD   & 0xFFF0 ) | ( ( i.integer & 0x0F ) << 0 );
		PB.DR.WORD   = ( PB.DR.WORD   & 0xFFC3 ) | ( ( i.integer & 0xF0 ) >> 2 );
		watch_dog = 0;
		break;
	default:
		break;
	}
	return 0;
}

// /////////////////////////////////////////////////////////
main(  )
{
	int i;
	int len;
	int channel;
	short counter_buf2[2];
	unsigned short analog[9];

	param_change = 0;
	servo_level = SERVO_LEVEL_STOP;
	watch_dog = 0;
	p_watch_dog_limit = 100;
	channel = 0;
	analog_mask = 0;
	dio_enable = 0;
	speed = 0;
	cnt_updated = 0;

	/* 初期化 */
	init(  );

	counter_buf2[0] = 0;
	counter_buf2[1] = 0;

	/* 受信開始 */
	sci_start(  );								// start SCI

	/* サーボループ始動 */
	CMT.CMSTR.BIT.STR1 = 1;						// CMT1 start

	while( 1 )
	{
		/* コマンド受信 */
		if( ( len = sci_receive( 0 ) ) > 0 )
		{
			channel = 0;
			command_analyze( SCI_receive_data[0], len );
		}
		if( ( len = sci_receive( 1 ) ) > 0 )
		{
			channel = 1;
			command_analyze( SCI_receive_data[1], len );
		}

		/* オドメトリ出力 */
		if( cnt_updated >= 5 )
		{
			unsigned short mask;
			/* 約5msおき */
			cnt_updated = 0;

			analog[0] = ( 0 << 12 ) | ( AD0.ADDRA.WORD >> 6 );
			analog[1] = ( 1 << 12 ) | ( AD0.ADDRB.WORD >> 6 );
			analog[2] = ( 2 << 12 ) | ( AD0.ADDRC.WORD >> 6 );
			analog[3] = ( 3 << 12 ) | ( AD0.ADDRD.WORD >> 6 );
			analog[4] = ( 4 << 12 ) | ( AD1.ADDRA.WORD >> 6 );
			analog[5] = ( 5 << 12 ) | ( AD1.ADDRB.WORD >> 6 );
			analog[6] = ( 6 << 12 ) | ( AD1.ADDRC.WORD >> 6 );
			analog[7] = ( 7 << 12 ) | ( AD1.ADDRD.WORD >> 6 );
			analog[8] = ( 15 << 12 ) | ( PE.DR.WORD & 0x0F ) | ( ( PB.DR.WORD  & 0x3C ) << 2 );
			mask = analog_mask;
			if( dio_enable ){
				mask |= 1 << 8;
			}

			data_send( channel,
					   ( short )( ( short )counter_buf[0]
								  - ( short )counter_buf2[0] ),
					   ( short )( ( short )counter_buf[1] - ( short )counter_buf2[1] ), pwm_buf[0], pwm_buf[1], analog,
					   mask );

			counter_buf2[0] = counter_buf[0];
			counter_buf2[1] = counter_buf[1];

		}
		if( watch_dog > p_watch_dog_limit )
		{
			param_change = 0;
			watch_dog = 0;
			p_watch_dog_limit = 100;
			channel = 0;
			analog_mask = 0;
			cnt_updated = 0;
			servo_level = SERVO_LEVEL_STOP;
			if( speed != 38400 )
			{
				sci_init( 38400 );
				sci_start(  );					// start SCI
			}
		}
	}
}
