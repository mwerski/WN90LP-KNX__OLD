#pragma once

#define paramDelay(time) (uint32_t)( \
            (time & 0xC000) == 0xC000 ? (time & 0x3FFF) * 100 : \
            (time & 0xC000) == 0x0000 ? (time & 0x3FFF) * 1000 : \
            (time & 0xC000) == 0x4000 ? (time & 0x3FFF) * 60000 : \
            (time & 0xC000) == 0x8000 ? ((time & 0x3FFF) > 1000 ? 3600000 : \
                                            (time & 0x3FFF) * 3600000 ) : 0 )
//--------------------Allgemein---------------------------
#define MAIN_OpenKnxId 0xAF
#define MAIN_ApplicationNumber 0x00
#define MAIN_ApplicationVersion 0x01
#define MAIN_OrderNumber "p3-00012.1"
#define MAIN_ParameterSize 4
#define MAIN_MaxKoNumber 1


#define APP_startUp_Delay		0x0000
#define APP_startUp_Delay_Shift	3
#define APP_startUp_Delay_Mask	0x001F
// Offset: 0, Size: 5 Bit, Text: Verzögerung beim Start
#define ParamAPP_startUp_Delay ((uint32_t)((knx.paramByte(APP_startUp_Delay) >> APP_startUp_Delay_Shift) & APP_startUp_Delay_Mask))
#define APP_Heartbeat		0x0001
#define APP_Heartbeat_Shift	4
#define APP_Heartbeat_Mask	0x0FFF
// Offset: 1, Size: 12 Bit, Text: "In Betrieb" senden alle
#define ParamAPP_Heartbeat ((uint32_t)((knx.paramWord(APP_Heartbeat) >> APP_Heartbeat_Shift) & APP_Heartbeat_Mask))
#define APP_Date/Time_DPTs		0x0000
// Offset: 0, BitOffset: 5, Size: 1 Bit, Text: Zeit empfangen über
#define ParamAPP_Date/Time_DPTs knx.paramBit(APP_Date/Time_DPTs, 5)
#define APP_Watchdog		0x0000
// Offset: 0, BitOffset: 6, Size: 1 Bit, Text: Watchdog aktivieren
#define ParamAPP_Watchdog knx.paramBit(APP_Watchdog, 6)
#define APP_1wire_vorhanden?		0x0000
// Offset: 0, BitOffset: 7, Size: 1 Bit, Text: 1wire Sensor vorhanden
#define ParamAPP_1wire_vorhanden? knx.paramBit(APP_1wire_vorhanden?, 7)
#define APP_Feinstaubsensor_vorhanden?		0x0003
// Offset: 3, Size: 1 Bit, Text: Feinstaubsensor vorhanden
#define ParamAPP_Feinstaubsensor_vorhanden? knx.paramBit(APP_Feinstaubsensor_vorhanden?, 0)
#define APP_Uhrzeit_beim_Start_lesen?		0x0003
// Offset: 3, BitOffset: 1, Size: 1 Bit, Text: Uhrzeit beim Start lesen
#define ParamAPP_Uhrzeit_beim_Start_lesen? knx.paramBit(APP_Uhrzeit_beim_Start_lesen?, 1)
//!< Number: 1, Text: In Betrieb, Function: Statusmeldung
#define APP_KoHeartbeat 1
#define KoAPP_Heartbeat knx.getGroupObject(APP_KoHeartbeat)
//!< Number: 0, Text: Fehlermeldung, Function: 
#define APP_KoFehlermeldung 0
#define KoAPP_Fehlermeldung knx.getGroupObject(APP_KoFehlermeldung)
//!< Number: 0, Text: Zeit, Function: Eingang
#define APP_KoTime 0
#define KoAPP_Time knx.getGroupObject(APP_KoTime)
//!< Number: 0, Text: Datum, Function: Eingang
#define APP_KoDate 0
#define KoAPP_Date knx.getGroupObject(APP_KoDate)
//!< Number: 0, Text: Datum/Zeit, Function: Eingang
#define APP_KoDate/Time 0
#define KoAPP_Date/Time knx.getGroupObject(APP_KoDate/Time)

