// vibe_sonate2_tlm.h — V12 beta90 1.3: SONATE-2's 9k6 AX.25 telemetry → named values (docs/v12/SATELLITE-BEST-SERVER.md).
//
// ★★★ SOURCE: University of Würzburg, "SONATE-2 protocol definition for radio amateurs" (the spreadsheet linked from the
//     mission's "Information for radio amateurs" page, fetched 2026-11-21): AX.25 UI frames from DP0SNX (PID 0xF0), each
//     information field ONE CCSDS transfer frame (132.0-B: version 0, spacecraft id 0x270, VCID 0–7, MCFC/VCFC, FHP),
//     carrying CCSDS space packets (133.0-B; secondary header 5 bytes on VC 0–3: unix time 32, CRC-OK 1, interval 7), the
//     frame closed by a CRC-16 (poly 0x1021, init 0xFFFF, no final XOR). APID 100 "STD HK" (111 bytes) is laid out field by
//     field with bit positions and the mission's OWN CALIBRATIONS (polynomials, enumerations) — the table below is
//     generated from that sheet (scratch tool, not committed; regenerate from the sheet if the mission revises it).
// ★ Only APID 100 is turned into values; other APIDs are counted and shown as hex. A space packet split across transfer
//   frames is not reassembled (APID 100 fits one frame). A frame is used only if its CRC, version and spacecraft id agree.
// ★ Content: SONATE-2 is an amateur-band university satellite asking amateurs to receive it — its housekeeping is shown.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vibesonate2 {

struct Field { const char* key; const char* name; const char* unit; int bits; int pos; bool cal; double c0, c1, c2; const char* enums; };

/** ★ APID 100 STD HK — generated from the mission's sheet ("DATA definition", APID 100), in its order. */
inline const std::vector<Field>& stdHk() {
    static const std::vector<Field> t = {
    { "O1UPTM", "OBDH1 UPTIME", "sec", 24, 0, false, 0, 0, 0, "" },
    { "O1RSTC", "OBDH1 RESET COUNTER", "", 6, 24, false, 0, 0, 0, "" },
    { "O1CMDC", "OBDH1 CMD COUNTER", "", 4, 30, false, 0, 0, 0, "" },
    { "O1PWRS", "OBDH1 POWER STATUS", "", 1, 34, false, 0, 0, 0, "0=OFF;1=ON" },
    { "O1ROLE", "OBDH1 ROLE", "", 3, 35, false, 0, 0, 0, "0=NO ROLE;4=OBDH PASSIVE 3;5=OBDH PASSIVE 2;6=OBDH PASSIVE 1;7=OBDH ACTIVE;3=PDH ACTIVE;1=BOOTLOADER" },
    { "O1ECNT", "OBDH1 ERROR COUNTER", "", 6, 38, false, 0, 0, 0, "" },
    { "O1ECDE", "OBDH1 ERROR CODE", "", 6, 44, false, 0, 0, 0, "1=CONFIG;2=ROLE MANAGER;3=NFM CONTROLLER;4=STD TC EXECUTER;5=STD TC RECEIVER;6=STD TC SAVER;7=SOFTWARE WDG;8=OBDH TC RECEIVER;9=SYSTEM MANAGER;10=CAN 1;11=CAN 2;12=SYNCH 1;13=SYNCH 2;14=SYNCH 3;15=SYNCH 4;16=LINKINTERFACE CAN 1;17=LINKINTERFACE CAN 2;0=NO ERROR;18=HOUSEKEEPER;19=TM DOWNLINK;20=STORAGE MONITOR;21=PARALLEL NOR 1;22=PARALLEL NOR 2;23=SPI NOR;24=SRAM 1;25=SRAM 2;26=SRAM CONTROLLER;27=PAYLOAD HANDLER;28=SOFTWARE INSTALLER;29=MEMOP MANAGER;30=SOFTWARE UPLOAD;31=REPAIR AND CHECK;32=VHF 1;33=VHF 2;34=UHF 1;35=UHF 2;36=EGSE 1;37=EGSE 2;38=EXP DATA HANDLER;39=SAFE MODE LIMIT;50=BTLDR NO ERROR;51=BTLDR INVLD ADDR;52=BTLDR INVLD SECTOR;53=BTLDR INVLD SIZE;54=BTLDR INVLD CMD;55=BTLDR UNKNOWN CMD;56=BTLDR CAN OVFLW;57=BTLDR INVLD CRC;58=BTLDR APP IMG DEFECT;40=SBAND;41=BOOT MSG HANDLER;59=BTLDR RDP L1 SET;60=BTLDR RDP L2 SET;61=BTLDR WRP SET;42=CAN LISTENER;63=BTLDR DEFECT" },
    { "O2UPTM", "OBDH2 UPTIME", "sec", 24, 50, false, 0, 0, 0, "" },
    { "O2RSTC", "OBDH2 RESET COUNTER", "", 6, 74, false, 0, 0, 0, "" },
    { "O2CMDC", "OBDH2 CMD COUNTER", "", 4, 80, false, 0, 0, 0, "" },
    { "O2PWRS", "OBDH2 POWER STATUS", "", 1, 84, false, 0, 0, 0, "0=OFF;1=ON" },
    { "O2ROLE", "OBDH2 ROLE", "", 3, 85, false, 0, 0, 0, "0=NO ROLE;4=OBDH PASSIVE 3;5=OBDH PASSIVE 2;6=OBDH PASSIVE 1;7=OBDH ACTIVE;3=PDH ACTIVE;1=BOOTLOADER" },
    { "O2ECNT", "OBDH2 ERROR COUNTER", "", 6, 88, false, 0, 0, 0, "" },
    { "O2ECDE", "OBDH2 ERROR CODE", "", 6, 94, false, 0, 0, 0, "1=CONFIG;2=ROLE MANAGER;3=NFM CONTROLLER;4=STD TC EXECUTER;5=STD TC RECEIVER;6=STD TC SAVER;7=SOFTWARE WDG;8=OBDH TC RECEIVER;9=SYSTEM MANAGER;10=CAN 1;11=CAN 2;12=SYNCH 1;13=SYNCH 2;14=SYNCH 3;15=SYNCH 4;16=LINKINTERFACE CAN 1;17=LINKINTERFACE CAN 2;0=NO ERROR;18=HOUSEKEEPER;19=TM DOWNLINK;20=STORAGE MONITOR;21=PARALLEL NOR 1;22=PARALLEL NOR 2;23=SPI NOR;24=SRAM 1;25=SRAM 2;26=SRAM CONTROLLER;27=PAYLOAD HANDLER;28=SOFTWARE INSTALLER;29=MEMOP MANAGER;30=SOFTWARE UPLOAD;31=REPAIR AND CHECK;32=VHF 1;33=VHF 2;34=UHF 1;35=UHF 2;36=EGSE 1;37=EGSE 2;39=SAFE MODE LIMIT;38=EXP DATA HANDLER;50=BTLDR NO ERROR;51=BTLDR INVLD ADDR;52=BTLDR INVLD SECTOR;53=BTLDR INVLD SIZE;54=BTLDR INVLD CMD;55=BTLDR UNKNOWN CMD;56=BTLDR CAN OVFLW;57=BTLDR INVLD CRC;58=BTLDR APP IMG DEFECT;40=SBAND;41=BOOT MSG HANDLER;59=BTLDR RDP L1 SET;60=BTLDR RDP L2 SET;61=BTLDR WRP SET;42=CAN LISTENER;63=BTLDR DEFECT" },
    { "O3UPTM", "OBDH3 UPTIME", "sec", 24, 100, false, 0, 0, 0, "" },
    { "O3RSTC", "OBDH3 RESET COUNTER", "", 6, 124, false, 0, 0, 0, "" },
    { "O3CMDC", "OBDH3 CMD COUNTER", "", 4, 130, false, 0, 0, 0, "" },
    { "O3PWRS", "OBDH3 POWER STATUS", "", 1, 134, false, 0, 0, 0, "0=OFF;1=ON" },
    { "O3ROLE", "OBDH3 ROLE", "", 3, 135, false, 0, 0, 0, "0=NO ROLE;4=OBDH PASSIVE 3;5=OBDH PASSIVE 2;6=OBDH PASSIVE 1;7=OBDH ACTIVE;3=PDH ACTIVE;1=BOOTLOADER" },
    { "O3ECNT", "OBDH3 ERROR COUNTER", "", 6, 138, false, 0, 0, 0, "" },
    { "O3ECDE", "OBDH3 ERROR CODE", "", 6, 144, false, 0, 0, 0, "1=CONFIG;2=ROLE MANAGER;3=NFM CONTROLLER;4=STD TC EXECUTER;5=STD TC RECEIVER;6=STD TC SAVER;7=SOFTWARE WDG;8=OBDH TC RECEIVER;9=SYSTEM MANAGER;10=CAN 1;11=CAN 2;12=SYNCH 1;13=SYNCH 2;14=SYNCH 3;15=SYNCH 4;16=LINKINTERFACE CAN 1;17=LINKINTERFACE CAN 2;0=NO ERROR;18=HOUSEKEEPER;19=TM DOWNLINK;20=STORAGE MONITOR;21=PARALLEL NOR 1;22=PARALLEL NOR 2;23=SPI NOR;24=SRAM 1;25=SRAM 2;26=SRAM CONTROLLER;27=PAYLOAD HANDLER;28=SOFTWARE INSTALLER;29=MEMOP MANAGER;30=SOFTWARE UPLOAD;31=REPAIR AND CHECK;32=VHF 1;33=VHF 2;34=UHF 1;35=UHF 2;36=EGSE 1;37=EGSE 2;38=EXP DATA HANDLER;39=SAFE MODE LIMIT;50=BTLDR NO ERROR;51=BTLDR INVLD ADDR;52=BTLDR INVLD SECTOR;53=BTLDR INVLD SIZE;54=BTLDR INVLD CMD;55=BTLDR UNKNOWN CMD;56=BTLDR CAN OVFLW;57=BTLDR INVLD CRC;58=BTLDR APP IMG DEFECT;40=SBAND;41=BOOT MSG HANDLER;59=BTLDR RDP L1 SET;60=BTLDR RDP L2 SET;61=BTLDR WRP SET;42=CAN LISTENER;63=BTLDR DEFECT" },
    { "O4UPTM", "OBDH4 UPTIME", "sec", 24, 150, false, 0, 0, 0, "" },
    { "O4RSTC", "OBDH4 RESET COUNTER", "", 6, 174, false, 0, 0, 0, "" },
    { "O4CMDC", "OBDH4 CMD COUNTER", "", 4, 180, false, 0, 0, 0, "" },
    { "O4PWRS", "OBDH4 POWER STATUS", "", 1, 184, false, 0, 0, 0, "0=OFF;1=ON" },
    { "O4ROLE", "OBDH4 ROLE", "", 3, 185, false, 0, 0, 0, "0=NO ROLE;4=OBDH PASSIVE 3;5=OBDH PASSIVE 2;6=OBDH PASSIVE 1;7=OBDH ACTIVE;3=PDH ACTIVE;1=BOOTLOADER" },
    { "O4ECNT", "OBDH4 ERROR COUNTER", "", 6, 188, false, 0, 0, 0, "" },
    { "O4ECDE", "OBDH4 ERROR CODE", "", 6, 194, false, 0, 0, 0, "1=CONFIG;2=ROLE MANAGER;3=NFM CONTROLLER;4=STD TC EXECUTER;5=STD TC RECEIVER;6=STD TC SAVER;7=SOFTWARE WDG;8=OBDH TC RECEIVER;9=SYSTEM MANAGER;10=CAN 1;11=CAN 2;12=SYNCH 1;13=SYNCH 2;14=SYNCH 3;15=SYNCH 4;16=LINKINTERFACE CAN 1;17=LINKINTERFACE CAN 2;0=NO ERROR;18=HOUSEKEEPER;19=TM DONWLINK;20=STORAGE MONITOR;21=PARALLEL NOR 1;22=PARALLEL NOR 2;23=SPI NOR;24=SRAM 1;25=SRAM 2;26=SRAM CONTROLLER;27=PAYLOAD HANDLER;28=SOFTWARE INSTALLER;29=MEMOP MANAGER;30=SOFTWARE UPLOAD;31=REPAIR AND CHECK;32=VHF 1;33=VHF 2;34=UHF 1;35=UHF 2;36=EGSE 1;37=EGSE 2;38=EXP DATA HANDLER;39=SAFE MODE LIMIT;50=BTLDR NO ERROR;51=BTLDR INVLD ADDR;52=BTLDR INVLD SECTOR;53=BTLDR INVLD SIZE;54=BTLDR INVLD CMD;55=BTLDR UNKNOWN CMD;56=BTLDR CAN OVFLW;57=BTLDR INVLD CRC;58=BTLDR APP IMG DEFECT;40=SBAND;41=BOOT MSG HANDLER;59=BTLDR RDP L1 SET;60=BTLDR RDP L2 SET;61=BTLDR WRP SET;42=CAN LISTENER;63=BTLDR DEFECT" },
    { "PDHHWI", "PDH HW ID", "", 3, 200, false, 0, 0, 0, "0=NONE;4=OBDH 1;5=OBDH 2;6=OBDH 3;7=OBDH 4" },
    { "CMDACN", "ACTIVE TT LIST COUNTER", "", 12, 203, false, 0, 0, 0, "" },
    { "CMDPCN", "PASSIVE TT LIST COUNTER", "", 12, 215, false, 0, 0, 0, "" },
    { "CMDEEC", "CMD EXECUTION ERROR COUNT", "", 4, 227, false, 0, 0, 0, "" },
    { "CMDEED", "CMD EXECUTION ERROR CODE", "", 4, 231, false, 0, 0, 0, "0=NO ERROR;1=CMD NOT SUPPORTED;2=CMD OVERDUE WARNING;3=CMD OVERDUE ERROR;4=CMD CRC ERROR;5=CMD FLASH BUSY;6=CMD TT ERROR;7=CMD INVALID SINK;8=CMD EXEC FAILED BOTH;9=CMD INVALID LENGTH;10=CMD INVALID TYPE;11=CMD INVALID CODE;12=CMD INVALID EXE SINK;13=CMD EXEC FAILED BUS1;14=CMD EXEC FAILED BUS2" },
    { "CMDSRC", "LAST TC SOURCE", "", 4, 235, false, 0, 0, 0, "8=NONE;4=EGSE 1;0=VHF 1;1=VHF 2;2=UHF 1;3=UHF 2;6=S-BAND 1;7=S-BAND 2;5=EGSE 2" },
    { "CMDECE", "CMD COUNTER ERROR COUNTER", "", 4, 239, false, 0, 0, 0, "" },
    { "CAN1RS", "CAN-BUS 1 RX STATE", "", 4, 243, false, 0, 0, 0, "1=NORMAL;2=WARN SOFT;3=WARN CRITIC;4=PASSIVE;5=OFF;7=NULLPTR;0=NOT INIT" },
    { "CAN1TS", "CAN-BUS 1 TX STATE", "", 4, 247, false, 0, 0, 0, "1=NORMAL;2=WARN SOFT;3=WARN CRITIC;4=PASSIVE;5=OFF;7=NULLPTR;0=NOT INIT" },
    { "CAN2RS", "CAN-BUS 2 RX STATE", "", 4, 251, false, 0, 0, 0, "1=NORMAL;2=WARN SOFT;3=WARN CRITIC;4=PASSIVE;5=OFF;7=NULLPTR;0=NOT INIT" },
    { "CAN2TS", "CAN-BUS 2 TX STATE", "", 4, 255, false, 0, 0, 0, "1=NORMAL;2=WARN SOFT;3=WARN CRITIC;4=PASSIVE;5=OFF;7=NULLPTR;0=NOT INIT" },
    { "OBMODE", "OBDH MODE", "", 4, 259, false, 0, 0, 0, "0=BOOT MODE;2=LEOP MODE;1=SAFE MODE;3=NORMAL MODE;4=PROCEDURE MODE;6=USER MODE;15=SIMULATOR;5=BEACON MODE;14=OBC LIGHT UHF;13=OBC LIGHT VHF" },
    { "TMSINK", "CURRENT TM SINK", "", 4, 263, false, 0, 0, 0, "0=VHF 1;1=VHF 2;2=UHF 1;3=UHF 2;8=NONE;4=EGSE 1;5=EGSE 2;6=S-BAND 1;7=S-BAND 2;10=VHF 1 + S-BAND;11=VHF 2 + S-BAND;12=UHF 1 + S-BAND;13=UHF 2 + S-BAND;14=EGSE 1 + S-BAND;15=EGSE 2 + S-BAND" },
    { "GPCTHW", "GPIO CONTROLLER HW ID", "", 3, 267, false, 0, 0, 0, "0=OBDH 1;1=OBDH 2;2=OBDH 3;3=OBDH 4;7=NONE" },
    { "SWUPHW", "SW UPLOAD RCVR HW ID", "", 3, 270, false, 0, 0, 0, "0=NONE;4=OBDH 1;5=OBDH 2;6=OBDH 3;7=OBDH 4" },
    { "SWINHW", "SW INSTALLER HW ID", "", 3, 273, false, 0, 0, 0, "0=NONE;4=OBDH 1;5=OBDH 2;6=OBDH 3;7=OBDH 4" },
    { "SBCTHW", "S-BAND CONTROLLER HW ID", "", 3, 276, false, 0, 0, 0, "0=NONE;4=OBDH 1;5=OBDH 2;6=OBDH 3;7=OBDH 4" },
    { "CMDCNT", "SAT CMD COUNTER", "", 12, 279, false, 0, 0, 0, "" },
    { "VHF1MO", "VHF1 MODE", "", 3, 291, false, 0, 0, 0, "0=N/A;2=SSTV;3=CW;1=TMTC;4=APRS;5=DEPLOY;6=DEPLOY OVERRIDE" },
    { "VHF1TC", "VHF1 APRS TX COUNTER", "", 5, 294, false, 0, 0, 0, "" },
    { "VHF1TM", "VHF1 TEMPERATURE", "\xc2\xb0" "C", 8, 299, true, -128, 1, 0, "" },
    { "VHF1RS", "VHF1 RSSI RX", "dBm", 8, 307, true, -256, 1, 0, "" },
    { "VHF1RA", "VHF1 RSSI AVG", "dBm", 8, 315, true, -256, 1, 0, "" },
    { "VHF1TO", "VHF1 TX PWR", "dBm", 4, 323, false, 0, 0, 0, "15=31.5;14=31.4;13=31.0;12=30.0;11=28.5;10=28.2;9=28.1;8=27.4;7=26.0;6=24.0;5=21.2;4=16.5;3=7.0;2=N/A;1=N/A;0=0 - PA OFF" },
    { "VHF1EC", "VHF1 ERROR CODE", "", 5, 327, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=I2C ERROR;4=TX PACKET ORDER;5=TX PACKET TIMEOUT;6=TX PACKET OVERRUN;7=AUTORANGING RX;8=AUTORANGING TX;9=RX CRC FAIL;10=RX ABORT;11=RX SIZE FAIL;12=RX ADDR FAIL;13=CFG REPAIR 1-2 GOOD;14=RX PACKET OVERRUN;15=APRS RX BUF OVERRUN;16=PREP COMPLETE;17=TC DEC UNEXP LEN;18=TC DEC CRC ERR;19=CAN RX BUF OVERRUN;20=CAN BUSY TIMEOUT;21=UNEXP IMG UPL STATE;22=ILL IMG UPL START;23=IMG UPL ABORT;24=IMG UPL PKT CRC;25=IMG UPL CRC;26=TRX FAIL;27=DEPLOY TIMEOUT;28=CFG TRIPLE RED SUCC;29=CFG TRIPLE RED FAIL;30=OBCL CAN REC OVERRUN;31=UNEXP RX FIFO ERR" },
    { "VHF1CC", "VHF1 CMD COUNTER", "", 4, 332, false, 0, 0, 0, "" },
    { "VHF1EN", "VHF1 ERROR COUNTER", "", 4, 336, false, 0, 0, 0, "" },
    { "VHF1RC", "VHF1 AX25 RX COUNTER", "", 5, 340, false, 0, 0, 0, "" },
    { "VHF2MO", "VHF2 MODE", "", 3, 345, false, 0, 0, 0, "0=N/A;1=TMTC;2=SSTV;3=CW;4=APRS;5=DEPLOY;6=DEPLOY_OVERRIDE" },
    { "VHF2TC", "VHF2 APRS TX COUNTER", "", 5, 348, false, 0, 0, 0, "" },
    { "VHF2TM", "VHF2 TEMPERATURE", "\xc2\xb0" "C", 8, 353, true, -128, 1, 0, "" },
    { "VHF2RS", "VHF2 RSSI RX", "dBm", 8, 361, true, -256, 1, 0, "" },
    { "VHF2RA", "VHF2 RSSI AVG", "dBm", 8, 369, true, -256, 1, 0, "" },
    { "VHF2TO", "VHF2 TX PWR", "dBm", 4, 377, false, 0, 0, 0, "15=31.7;14=31.6;13=31.4;12=30.9;11=29.7;10=29.6;9=29.5;8=28.9;7=28.1;6=26.7;5=24.9;4=21.9;3=16.8;2=7.0;1=N/A;0=0 - PA OFF" },
    { "VHF2EC", "VHF2 ERROR CODE", "", 5, 381, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=I2C ERROR;4=TX PACKET ORDER;5=TX PACKET TIMEOUT;6=TX PACKET OVERRUN;7=AUTORANGING RX;8=AUTORANGING TX;9=RX CRC FAIL;10=RX ABORT;11=RX SIZE FAIL;12=RX ADDR FAIL;13=CFG REPAIR 1-2 GOOD;14=RX PACKET OVERRUN;15=APRS RX BUF OVERRUN;16=PREP COMPLETE;17=TC DEC UNEXP LEN;18=TC DEC CRC ERR;19=CAN RX BUF OVERRUN;20=CAN BUSY TIMEOUT;21=UNEXP IMG UPL STATE;22=ILL IMG UPL START;23=IMG UPL ABORT;24=IMG UPL PKT CRC;25=IMG UPL CRC;26=TRX FAIL;27=DEPLOY TIMEOUT;28=CFG TRIPLE RED SUCC;29=CFG TRIPLE RED FAIL;30=OBCL CAN REC OVERRUN;31=UNEXP RX FIFO ERR" },
    { "VHF2CC", "VHF2 CMD COUNTER", "", 4, 386, false, 0, 0, 0, "" },
    { "VHF2EN", "VHF2 ERROR COUNTER", "", 4, 390, false, 0, 0, 0, "" },
    { "VHF2RC", "VHF2 AX25 RX COUNTER", "", 5, 394, false, 0, 0, 0, "" },
    { "UHF1MO", "UHF1 MODE", "", 3, 399, false, 0, 0, 0, "0=N/A;2=SSTV;3=CW;1=TMTC;4=APRS;5=DEPLOY;6=DEPLOY OVERRIDE" },
    { "UHF1TM", "UHF1 TEMPERATURE", "\xc2\xb0" "C", 8, 402, true, -128, 1, 0, "" },
    { "UHF1RS", "UHF1 RSSI RX", "dBm", 8, 410, true, -256, 1, 0, "" },
    { "UHF1RA", "UHF1 RSSI AVG", "dBm", 8, 418, true, -256, 1, 0, "" },
    { "UHF1TO", "UHF1 TX PWR", "dBm", 4, 426, false, 0, 0, 0, "15=30.6;14=30.0;13=28.8;12=27.7;11=25.7;10=24.4;9=22.5;8=18.3;7=3.0;6=N/A;5=N/A;4=N/A;3=N/A;2=N/A;1=N/A;0=0 - PA OFF" },
    { "UHF1EC", "UHF1 ERROR CODE", "", 5, 430, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=I2C ERROR;4=TX PACKET ORDER;5=TX PACKET TIMEOUT;6=TX PACKET OVERRUN;7=AUTORANGING RX;8=AUTORANGING TX;9=RX CRC FAIL;10=RX ABORT;11=RX SIZE FAIL;12=RX ADDR FAIL;13=CFG REPAIR 1-2 GOOD;14=RX PACKET OVERRUN;15=APRS RX BUF OVERRUN;16=PREP COMPLETE;17=TC DEC UNEXP LEN;18=TC DEC CRC ERR;19=CAN RX BUF OVERRUN;20=CAN BUSY TIMEOUT;21=UNEXP IMG UPL STATE;22=ILL IMG UPL START;23=IMG UPL ABORT;24=IMG UPL PKT CRC;25=IMG UPL CRC;26=TRX FAIL;27=DEPLOY TIMEOUT;28=CFG TRIPLE RED SUCC;29=CFG TRIPLE RED FAIL;30=OBCL CAN REC OVERRUN;31=UNEXP RX FIFO ERR" },
    { "UHF1CC", "UHF1 CMD COUNTER", "", 4, 435, false, 0, 0, 0, "" },
    { "UHF1EN", "UHF1 ERROR COUNTER", "", 4, 439, false, 0, 0, 0, "" },
    { "UHF1RC", "UHF1 AX25 RX COUNTER", "", 5, 443, false, 0, 0, 0, "" },
    { "UHF2MO", "UHF2 MODE", "", 3, 448, false, 0, 0, 0, "0=N/A;2=SSTV;3=CW;1=TMTC;4=APRS;5=DEPLOY;6=DEPLOY OVERRIDE" },
    { "UHF2TM", "UHF2 TEMPERATURE", "\xc2\xb0" "C", 8, 451, true, -128, 1, 0, "" },
    { "UHF2RS", "UHF2 RSSI RX", "dBm", 8, 459, true, -256, 1, 0, "" },
    { "UHF2RA", "UHF2 RSSI AVG", "dBm", 8, 467, true, -256, 1, 0, "" },
    { "UHF2TO", "UHF2 TX PWR", "dBm", 4, 475, false, 0, 0, 0, "15=30.7;14=30.4;13=29.5;12=28.4;11=27.1;10=26.2;9=24.3;8=20.7;7=7.8;6=N/A;5=N/A;4=N/A;3=N/A;2=N/A;1=N/A;0=0 - PA OFF" },
    { "UHF2EC", "UHF2 ERROR CODE", "", 5, 479, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=I2C ERROR;4=TX PACKET ORDER;5=TX PACKET TIMEOUT;6=TX PACKET OVERRUN;7=AUTORANGING RX;8=AUTORANGING TX;9=RX CRC FAIL;10=RX ABORT;11=RX SIZE FAIL;12=RX ADDR FAIL;13=CFG REPAIR 1-2 GOOD;14=RX PACKET OVERRUN;15=APRS RX BUF OVERRUN;16=PREP COMPLETE;17=TC DEC UNEXP LEN;18=TC DEC CRC ERR;19=CAN RX BUF OVERRUN;20=CAN BUSY TIMEOUT;21=UNEXP IMG UPL STATE;22=ILL IMG UPL START;23=IMG UPL ABORT;24=IMG UPL PKT CRC;25=IMG UPL CRC;26=TRX FAIL;27=DEPLOY TIMEOUT;28=CFG TRIPLE RED SUCC;29=CFG TRIPLE RED FAIL;30=OBCL CAN REC OVERRUN;31=UNEXP RX FIFO ERR" },
    { "UHF2CC", "UHF2 CMD COUNTER", "", 4, 484, false, 0, 0, 0, "" },
    { "UHF2EN", "UHF2 ERROR COUNTER", "", 4, 488, false, 0, 0, 0, "" },
    { "UHF2RC", "UHF2 AX25 RX COUNTER", "", 5, 492, false, 0, 0, 0, "" },
    { "P5V1CC", "PCU05V1 CMD COUNTER", "", 4, 497, false, 0, 0, 0, "" },
    { "P5V1PG", "PCU05V1 CURR LIM PWR GOOD", "", 1, 501, false, 0, 0, 0, "0=YES;1=NO" },
    { "P5V1PF", "PCU05V1 CURR LIM PWR FLT", "", 1, 502, false, 0, 0, 0, "0=FAULT;1=NO FAULT" },
    { "P5V1UE", "PCU05V1 UPCNVRTR EN", "", 1, 503, false, 0, 0, 0, "1=OFF;0=ON" },
    { "P5V1PE", "PCU05V1 POWER ENABLE", "", 1, 504, false, 0, 0, 0, "1=OFF;0=ON" },
    { "P5V1EO", "PCU05V1 ERROR CODE", "", 4, 505, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=I2C ERR;5=CMD RX BUF OVERRUN;6=CFG TRIPLE RED SUCC;7=CFG TRIPLE RED FAIL;8=PWR NOT GOOD 5V->12V" },
    { "P5V1EC", "PCU05V1 ERROR COUNTER", "", 4, 509, false, 0, 0, 0, "" },
    { "P5V1AV", "PCU05V1 BATTERY VOLTAGE", "V", 8, 513, true, 0, 0.020070588, 0, "" },
    { "P5V1AC", "PCU05V1 BATTERY CURRENT", "A", 8, 521, true, -7.641924759, 0.059702537, 0, "" },
    { "P5V1UV", "PCU05V1 UPCNVRTR IN VLTG", "V", 8, 529, true, 0, 0.020813943, 0, "" },
    { "P5V1UC", "PCU05V1 UPCNVRTR IN CURR", "A", 8, 537, true, 0, 0.029734205, 0, "" },
    { "P5V1BV", "PCU05V1 BUS1 5V VOLTAGE", "V", 8, 545, true, 0, 0.024530719, 0, "" },
    { "P5V1BC", "PCU05V1 BUS1 5V CURRENT", "A", 8, 553, true, 0, 0.029734205, 0, "" },
    { "P5V2CC", "PCU05V2 CMD COUNTER", "", 4, 561, false, 0, 0, 0, "" },
    { "P5V2PG", "PCU05V2 CURR LIM PWR GOOD", "", 1, 565, false, 0, 0, 0, "0=YES;1=NO" },
    { "P5V2PF", "PCU05V2 CURR LIM PWR FLT", "", 1, 566, false, 0, 0, 0, "0=FAULT;1=NO FAULT" },
    { "P5V2UE", "PCU05V2 UPCNVRTR EN", "", 1, 567, false, 0, 0, 0, "1=OFF;0=ON" },
    { "P5V2PE", "PCU05V2 POWER ENABLE", "", 1, 568, false, 0, 0, 0, "1=OFF;0=ON" },
    { "P5V2EO", "PCU05V2 ERROR CODE", "", 4, 569, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=I2C ERR;5=CMD RX BUF OVERRUN;6=CFG TRIPLE RED SUCC;7=CFG TRIPLE RED FAIL;8=PWR NOT GOOD 5V->12V" },
    { "P5V2EC", "PCU05V2 ERROR COUNTER", "", 4, 573, false, 0, 0, 0, "" },
    { "P5V2AV", "PCU05V2 BATTERY VOLTAGE", "V", 8, 577, true, 0, 0.019882353, 0, "" },
    { "P5V2AC", "PCU05V2 BATTERY CURRENT", "A", 8, 585, true, -7.570253718, 0.059142607, 0, "" },
    { "P5V2UV", "PCU05V2 UPCNVRTR IN VLTG", "V", 8, 593, true, 0, 0.020618736, 0, "" },
    { "P5V2UC", "PCU05V2 UPCNVRTR IN CURR", "A", 8, 601, true, 0, 0.029455338, 0, "" },
    { "P5V2BV", "PCU05V2 BUS2 5V VOLTAGE", "V", 8, 609, true, 0, 0.024300654, 0, "" },
    { "P5V2BC", "PCU05V2 BUS2 5V CURRENT", "A", 8, 617, true, 0, 0.029455338, 0, "" },
    { "P5V1BT", "PCU05V1 BATTERY TEMP", "\xc2\xb0" "C", 8, 625, true, -47.23889465, 0.548218308, 1.95267e-05, "" },
    { "12V1BT", "PCU12V1 BATTERY TEMP", "\xc2\xb0" "C", 8, 633, true, -43.91789355, 0.534063389, 1.28733e-05, "" },
    { "SXP1PS", "SS XP1 POWER STATUS", "", 1, 641, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SXN1PS", "SS XN1 POWER STATUS", "", 1, 642, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SYP1PS", "SS YP1 POWER STATUS", "", 1, 643, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SYN1PS", "SS YN1 POWER STATUS", "", 1, 644, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SZP1PS", "SS ZP1 POWER STATUS", "", 1, 645, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SZN1PS", "SS ZN1 POWER STATUS", "", 1, 646, false, 0, 0, 0, "1=ON;0=OFF" },
    { "ADS1PS", "ADCS1 POWER STATUS", "", 1, 647, false, 0, 0, 0, "1=ON;0=OFF" },
    { "MVW1PS", "MULTIVIEW1 POWER STATUS", "", 1, 648, false, 0, 0, 0, "1=ON;0=OFF" },
    { "THR1PS", "THRUSTER1 POWER STATUS", "", 1, 649, false, 0, 0, 0, "1=ON;0=OFF" },
    { "IFP1PS", "IFP1 POWER STATUS", "", 1, 650, false, 0, 0, 0, "1=ON;0=OFF" },
    { "P5V1PS", "PCU05V1 POWER SYNC", "", 1, 651, false, 0, 0, 0, "0=RESET;1=SET" },
    { "P5V1HA", "PCU05V1 HK AVAILABLE", "", 1, 652, false, 0, 0, 0, "1=TRUE;0=FALSE" },
    { "THR1PG", "THRUSTER1 POWER GOOD", "", 1, 653, false, 0, 0, 0, "0=FAULT;1=GOOD" },
    { "P5V1RS", "PCU05V1 RELAIS STATUS", "", 1, 654, false, 0, 0, 0, "0=OFF;1=ON" },
    { "P5V1CS", "PCU05V1 CURRENT LIMIT STS", "", 1, 655, false, 0, 0, 0, "0=OFF;1=ON" },
    { "P5V111", "PCU05V1 MAX FRONT11 AV", "", 1, 656, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V121", "PCU05V1 MAX FRONT21 AV", "", 1, 657, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V122", "PCU05V1 MAX FRONT22 AV", "", 1, 658, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V1TR", "PCU05V1 MAX TERM AV", "", 1, 659, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V1RM", "PCU05V1 UPCNVRTR MODE", "", 1, 660, false, 0, 0, 0, "0=PFM;1=PWM" },
    { "P5V2BT", "PCU05V2 BATTERY TEMP", "\xc2\xb0" "C", 8, 661, true, -45.19886296, 0.553555222, 1.98982e-05, "" },
    { "12V2BT", "PCU12V2 BATTERY TEMP", "\xc2\xb0" "C", 8, 669, true, -44.08916192, 0.53363536, 1.28531e-05, "" },
    { "SXP2PS", "SS XP2 POWER STATUS", "", 1, 677, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SXN2PS", "SS XN2 POWER STATUS", "", 1, 678, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SYP2PS", "SS YP2 POWER STATUS", "", 1, 679, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SYN2PS", "SS YN2 POWER STATUS", "", 1, 680, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SZP2PS", "SS ZP2 POWER STATUS", "", 1, 681, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SZN2PS", "SS ZN2 POWER STATUS", "", 1, 682, false, 0, 0, 0, "1=ON;0=OFF" },
    { "ADS2PS", "ADCS2 POWER STATUS", "", 1, 683, false, 0, 0, 0, "1=ON;0=OFF" },
    { "MVW2PS", "MULTIVIEW2 POWER STATUS", "", 1, 684, false, 0, 0, 0, "1=ON;0=OFF" },
    { "THR2PS", "THRUSTER2 POWER STATUS", "", 1, 685, false, 0, 0, 0, "1=ON;0=OFF" },
    { "IFP2PS", "IFP2 POWER STATUS", "", 1, 686, false, 0, 0, 0, "1=ON;0=OFF" },
    { "P5V2PS", "PCU05V2 POWER SYNC", "", 1, 687, false, 0, 0, 0, "0=RESET;1=SET" },
    { "P5V2HA", "PCU05V2 HK AVAILABLE", "", 1, 688, false, 0, 0, 0, "1=TRUE;0=FALSE" },
    { "THR2PG", "THRUSTER2 POWER GOOD", "", 1, 689, false, 0, 0, 0, "0=FAULT;1=GOOD" },
    { "P5V2RS", "PCU05V2 RELAIS STATUS", "", 1, 690, false, 0, 0, 0, "0=OFF;1=ON" },
    { "P5V2CS", "PCU05V2 CURRENT LIMIT STS", "", 1, 691, false, 0, 0, 0, "0=OFF;1=ON" },
    { "P5V211", "PCU05V2 MAX FRONT11 AV", "", 1, 692, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V221", "PCU05V2 MAX FRONT21 AV", "", 1, 693, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V222", "PCU05V2 MAX FRONT22 AV", "", 1, 694, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V2TR", "PCU05V2 MAX TERM AV", "", 1, 695, false, 0, 0, 0, "0=NOT AVAILABLE;1=AVAILABLE" },
    { "P5V2RM", "PCU05V2 UPCNVRTR MODE", "", 1, 696, false, 0, 0, 0, "0=PFM;1=PWM" },
    { "12V1CC", "PCU12V1 CMD COUNTER", "", 4, 697, false, 0, 0, 0, "" },
    { "12V1PG", "PCU12V1 CURR LIM PWR GOOD", "", 1, 701, false, 0, 0, 0, "0=YES;1=NO" },
    { "12V1PF", "PCU12V1 CURR LIM PWR FLT", "", 1, 702, false, 0, 0, 0, "0=FAULT;1=NO FAULT" },
    { "12V1UE", "PCU12V1 UPCNVRTR EN", "", 1, 703, false, 0, 0, 0, "1=OFF;0=ON" },
    { "12V1PE", "PCU12V1 POWER ENABLE", "", 1, 704, false, 0, 0, 0, "1=OFF;0=ON" },
    { "12V1EO", "PCU12V1 ERROR CODE", "", 4, 705, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=CMD RX BUF OVERRUN;5=CFG TRIPLE RED SUCC;6=CFG TRIPLE RED FAIL" },
    { "12V1EC", "PCU12V1 ERROR COUNTER", "", 4, 709, false, 0, 0, 0, "" },
    { "12V1AV", "PCU12V1 BATTERY VOLTAGE", "V", 8, 713, true, 0, 0.019764706, 0, "" },
    { "12V1AC", "PCU12V1 BATTERY CURRENT", "A", 8, 721, true, -7.525459318, 0.058792651, 0, "" },
    { "12V1UV", "PCU12V1 UPCNVRTR IN VLTG", "V", 8, 729, true, 0, 0.020496732, 0, "" },
    { "12V1UC", "PCU12V1 UPCNVRTR IN CURR", "A", 8, 737, true, 0, 0.029281046, 0, "" },
    { "12V1BV", "PCU12V1 BUS1 12V VOLTAGE", "V", 8, 745, true, 0, 0.051930796, 0, "" },
    { "12V1BC", "PCU12V1 BUS1 12V CURRENT", "A", 8, 753, true, 0, 0.029281046, 0, "" },
    { "12V2CC", "PCU12V2 CMD COUNTER", "", 4, 761, false, 0, 0, 0, "" },
    { "12V2PG", "PCU12V2 CURR LIM PWR GOOD", "", 1, 765, false, 0, 0, 0, "0=YES;1=NO" },
    { "12V2PF", "PCU12V2 CURR LIM PWR FLT", "", 1, 766, false, 0, 0, 0, "0=FAULT;1=NO FAULT" },
    { "12V2UE", "PCU12V2 UPCNVRTR EN", "", 1, 767, false, 0, 0, 0, "1=OFF;0=ON" },
    { "12V2PE", "PCU12V2 POWER ENABLE", "", 1, 768, false, 0, 0, 0, "1=OFF;0=ON" },
    { "12V2EO", "PCU12V2 ERROR CODE", "", 4, 769, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=CMD RX BUF OVERRUN;5=CFG TRIPLE RED SUCC;6=CFG TRIPLE RED FAIL" },
    { "12V2EC", "PCU12V2 ERROR COUNTER", "", 4, 773, false, 0, 0, 0, "" },
    { "12V2AV", "PCU12V2 BATTERY VOLTAGE", "V", 8, 777, true, 0, 0.019823529, 0, "" },
    { "12V2AC", "PCU12V2 BATTERY CURRENT", "A", 8, 785, true, -7.547856518, 0.058967629, 0, "" },
    { "12V2UV", "PCU12V2 UPCNVRTR IN VLTG", "V", 8, 793, true, 0, 0.020557734, 0, "" },
    { "12V2UC", "PCU12V2 UPCNVRTR IN CURR", "A", 8, 801, true, 0, 0.029368192, 0, "" },
    { "12V2BV", "PCU12V2 BUS2 12V VOLTAGE", "V", 8, 809, true, 0, 0.052085352, 0, "" },
    { "12V2BC", "PCU12V2 BUS2 12V CURRENT", "A", 8, 817, true, 0, 0.029368192, 0, "" },
    { "12V1SS", "PCU12V1 SOLAR REG STATE", "", 2, 825, false, 0, 0, 0, "0=UNDEF;1=CHARGING;2=CHARGE COMPL;3=FAULT" },
    { "AI1PWS", "AI1 POWER STATUS", "", 1, 827, false, 0, 0, 0, "1=ON;0=OFF" },
    { "12V1HA", "PCU12V1 HK AVAILABLE", "", 1, 828, false, 0, 0, 0, "1=TRUE;0=FALSE" },
    { "12V1CS", "PCU12V1 CHARGE STATUS", "", 1, 829, false, 0, 0, 0, "1=OFF;0=ON" },
    { "12V1RM", "PCU12V1 UPCNVRTR MODE", "", 1, 830, false, 0, 0, 0, "0=PFM;1=PWM" },
    { "12V2SS", "PCU12V2 SOLAR REG STATE", "", 2, 831, false, 0, 0, 0, "0=UNDEF;1=CHARGING;2=CHARGE COMPL;3=FAULT" },
    { "AI2PWS", "AI2 POWER STATUS", "", 1, 833, false, 0, 0, 0, "1=ON;0=OFF" },
    { "12V2HA", "PCU12V2 HK AVAILABLE", "", 1, 834, false, 0, 0, 0, "1=TRUE;0=FALSE" },
    { "12V2CS", "PCU12V2 CHARGE STATUS", "", 1, 835, false, 0, 0, 0, "0=ON;1=OFF" },
    { "12V2RM", "PCU12V2 UPCNVRTR MODE", "", 1, 836, false, 0, 0, 0, "0=PFM;1=PWM" },
    { "IFP1ST", "IFP1 STATE", "", 2, 837, false, 0, 0, 0, "1=IDLE;2=DEPLOY_OVERRIDE;3=DEPLOY;0=OFF" },
    { "SB1RXS", "S-BAND1 RS422 RX STATE", "", 1, 839, false, 0, 0, 0, "0=ON;1=OFF" },
    { "SB1ENA", "S-BAND1 POWER", "", 1, 840, false, 0, 0, 0, "0=OFF;1=ON" },
    { "RWX1PS", "RW X1 POWER STATUS", "", 1, 841, false, 0, 0, 0, "1=ON;0=OFF" },
    { "RWY1PS", "RW Y1 POWER STATUS", "", 1, 842, false, 0, 0, 0, "1=ON;0=OFF" },
    { "RWZ1PS", "RW Z1 POWER STATUS", "", 1, 843, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SB1TXS", "S-BAND1 RS422 TX STATE", "", 1, 844, false, 0, 0, 0, "0=OFF;1=ON" },
    { "IFP1EN", "IFP1 ERROR COUNTER", "", 4, 845, false, 0, 0, 0, "" },
    { "IFP1EC", "IFP1 ERROR CODE", "", 4, 849, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=DEPLOY TIMEOUT;5=DEPLOY OVERCURRENT;6=TORQ X FAULT;7=TORQ Y FAULT;8=TORQ Z FAULT;9=TORQ X OVERCURRENT;10=TORQ Y OVERCURRENT;11=TORQ Z OVERCURRENT;13=CFG TRIPLE RED SUCC;14=CFG TRIPLE RED FAIL;15=DEPLOY FAULT;12=TORQ TIMEOUT" },
    { "IFP1CC", "IFP1 CMD COUNTER", "", 4, 853, false, 0, 0, 0, "" },
    { "IFP1AC", "IFP1 ADCS CMD COUNTER", "", 4, 857, false, 0, 0, 0, "" },
    { "IFP2ST", "IFP2 STATE", "", 2, 861, false, 0, 0, 0, "1=IDLE;2=DEPLOY_OVERRIDE;3=DEPLOY;0=OFF" },
    { "SB2RXS", "S-BAND2 RS422 RX STATE", "", 1, 863, false, 0, 0, 0, "0=ON;1=OFF" },
    { "SB2ENA", "S-BAND2 POWER", "", 1, 864, false, 0, 0, 0, "0=OFF;1=ON" },
    { "RWX2PS", "RW X2 POWER STATUS", "", 1, 865, false, 0, 0, 0, "1=ON;0=OFF" },
    { "RWY2PS", "RW Y2 POWER STATUS", "", 1, 866, false, 0, 0, 0, "1=ON;0=OFF" },
    { "RWZ2PS", "RW Z2 POWER STATUS", "", 1, 867, false, 0, 0, 0, "1=ON;0=OFF" },
    { "SB2TXS", "S-BAND2 RS422 TX STATE", "", 1, 868, false, 0, 0, 0, "0=OFF;1=ON" },
    { "IFP2EN", "IFP2 ERROR COUNTER", "", 4, 869, false, 0, 0, 0, "" },
    { "IFP2EC", "IFP2 ERROR CODE", "", 4, 873, false, 0, 0, 0, "0=NO ERROR;1=ILLEGAL CMD;2=ILLEGAL PARAM;3=CFG REPAIR 1-2 GOOD;4=DEPLOY TIMEOUT;5=DEPLOY OVERCURRENT;6=TORQ X FAULT;7=TORQ Y FAULT;8=TORQ Z FAULT;9=TORQ X OVERCURRENT;10=TORQ Y OVERCURRENT;11=TORQ Z OVERCURRENT;12=TORQ TIMEOUT;13=CFG TRIPLE RED SUCC;14=CFG TRIPLE RED FAIL;15=DEPLOY FAULT" },
    { "IFP2CC", "IFP2 CMD COUNTER", "", 4, 877, false, 0, 0, 0, "" },
    { "IFP2AC", "IFP2 ADCS CMD COUNTER", "", 4, 881, false, 0, 0, 0, "" },
    };
    return t;
}

inline uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) { c ^= uint16_t(d[i]) << 8; for (int k = 0; k < 8; ++k) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1); }
    return c;
}
inline uint64_t bitsAt(const uint8_t* d, size_t nBytes, int pos, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len; ++i) { const int b = pos + i; if ((size_t)(b >> 3) >= nBytes) return v; v = (v << 1) | ((d[b >> 3] >> (7 - (b & 7))) & 1u); }
    return v;
}
inline std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += char(c); } else if (c >= 0x20) o += char(c); }
    return o;
}
inline std::string enumText(const char* enums, uint64_t v) {
    const std::string e = enums;
    size_t i = 0;
    while (i < e.size()) {
        size_t semi = e.find(';', i); if (semi == std::string::npos) semi = e.size();
        const std::string item = e.substr(i, semi - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && std::strtoull(item.c_str(), nullptr, 10) == v && item.find_first_not_of("0123456789") == eq) return item.substr(eq + 1);
        i = semi + 1;
    }
    return "";
}

struct Result { bool ok = false; int vcid = -1, mcfc = 0, vcfc = 0, apid = -1; uint32_t created = 0; bool crcOk = false; std::string json; };

/** One AX.25 information field (a transfer frame) → `sat_tlm` JSON for its APID 100 packet ("" json when there is none).
 *  `sat` the satellite's name, `now` the server's unix time. */
inline Result decode(const std::string& info, const std::string& sat, double now) {
    Result r;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(info.data());
    const size_t n = info.size();
    if (n < 6 + 6 + 2) return r;
    if (crc16(d, n - 2) != uint16_t((d[n - 2] << 8) | d[n - 1])) return r;
    const unsigned tfvn = d[0] >> 6, scid = (unsigned)((d[0] & 0x3F) << 4 | d[1] >> 4);
    if (tfvn != 0 || scid != 0x270) return r;
    r.vcid = (d[1] >> 1) & 7; r.mcfc = d[2]; r.vcfc = d[3];
    const unsigned fhp = (unsigned)((d[4] & 7) << 8 | d[5]);
    r.ok = true;
    if (fhp == 0x7FF) return r;
    size_t p = 6 + fhp;
    const size_t end = n - 2;
    while (p + 6 <= end) {
        const unsigned shf = (d[p] >> 3) & 1, apid = (unsigned)((d[p] & 7) << 8 | d[p + 1]);
        const unsigned psc = (unsigned)((d[p + 2] & 0x3F) << 8 | d[p + 3]);
        const size_t len = (size_t)((d[p + 4] << 8) | d[p + 5]) + 1;
        if (p + 6 + len > end) break;   // ★ continues in the next frame: not reassembled
        const uint8_t* body = d + p + 6;
        if (apid == 100 && shf && (r.vcid <= 3) && len >= 5 + 111) {
            r.apid = 100;
            r.created = (uint32_t)body[0] << 24 | (uint32_t)body[1] << 16 | (uint32_t)body[2] << 8 | body[3];
            r.crcOk = body[4] >> 7;
            const uint8_t* hk = body + 5;
            char b[64];
            std::string j = "{\"type\":\"sat_tlm\",\"data\":{\"sat\":\"" + esc(sat) + "\",\"format\":\"sonate2-ax25\"";
            std::snprintf(b, sizeof b, ",\"t\":%.0f,\"frame\":\"STD HK\",\"seq\":%u", std::floor(now), psc); j += b;
            std::snprintf(b, sizeof b, ",\"created\":%u", r.created); j += b;
            const std::string mode = enumText("0=BOOT MODE;2=LEOP MODE;1=SAFE MODE;3=NORMAL MODE;4=PROCEDURE MODE;6=USER MODE;15=SIMULATOR;5=BEACON MODE;14=OBC LIGHT UHF;13=OBC LIGHT VHF",
                                              bitsAt(hk, 111, 259, 4));
            if (!mode.empty()) j += ",\"mode\":\"" + esc(mode) + "\"";
            j += ",\"values\":[";
            bool first = true;
            for (const Field& f : stdHk()) {
                const uint64_t raw = bitsAt(hk, 111, f.pos, f.bits);
                if (!first) j += ",";
                first = false;
                j += std::string("{\"k\":\"") + f.key + "\",\"name\":\"" + esc(f.name) + "\"";
                if (f.cal) {
                    const double x = double(raw), v = f.c0 + f.c1 * x + f.c2 * x * x;
                    std::snprintf(b, sizeof b, ",\"v\":%.3f", v); j += b;
                } else { std::snprintf(b, sizeof b, ",\"v\":%llu", (unsigned long long)raw); j += b; }
                if (f.unit[0]) j += std::string(",\"unit\":\"") + esc(f.unit) + "\"";
                if (f.enums[0]) { const std::string t = enumText(f.enums, raw); if (!t.empty()) j += ",\"text\":\"" + esc(t) + "\""; }
                j += "}";
            }
            j += "],\"hex\":\"";
            static const char* hx = "0123456789abcdef";
            for (size_t i = 0; i < 111; ++i) { j += hx[hk[i] >> 4]; j += hx[hk[i] & 15]; }
            r.json = j + "\"}}";
            return r;
        }
        p += 6 + len;
    }
    return r;
}

/** ★ For TESTS: our own transfer frame around one APID 100 packet of `hk` (111 bytes). */
inline std::string encodeStdHk(const uint8_t* hk111, uint32_t created, unsigned psc = 1) {
    std::vector<uint8_t> f;
    f.push_back(uint8_t(0x270 >> 4)); f.push_back(uint8_t((0x270 & 0xF) << 4 | (0 << 1) | 0));   // TFVN 0, SCID, VCID 0, OCFF 0
    f.push_back(7); f.push_back(9);                      // MCFC, VCFC
    f.push_back(uint8_t(0x18)); f.push_back(0);           // SHF 0, SYNC 0, POF 0, SLID 3, FHP 0
    const size_t len = 5 + 111;
    f.push_back(uint8_t(0x08 | (100 >> 8))); f.push_back(uint8_t(100 & 0xFF));   // PVN 0, PT 0, SHF 1, APID 100
    f.push_back(uint8_t(0xC0 | ((psc >> 8) & 0x3F))); f.push_back(uint8_t(psc & 0xFF));
    f.push_back(uint8_t((len - 1) >> 8)); f.push_back(uint8_t((len - 1) & 0xFF));
    for (int i = 3; i >= 0; --i) f.push_back(uint8_t(created >> (8 * i)));
    f.push_back(0x80 | 5);
    f.insert(f.end(), hk111, hk111 + 111);
    const uint16_t c = crc16(f.data(), f.size());
    f.push_back(uint8_t(c >> 8)); f.push_back(uint8_t(c & 0xFF));
    return std::string(f.begin(), f.end());
}

} // namespace vibesonate2
