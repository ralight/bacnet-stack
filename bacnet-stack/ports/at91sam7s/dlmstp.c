/**************************************************************************
*
* Copyright (C) 2007 Steve Karg <skarg@users.sourceforge.net>
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*
*********************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "bacdef.h"
#include "mstp.h"
#include "dlmstp.h"
#include "rs485.h"
#include "npdu.h"
/* This file has been customized for use with the AT91SAM7S-EK */
#include "board.h"

/* Number of MS/TP Packets Rx/Tx */
uint16_t MSTP_Packets = 0;

/* receive buffer */
static DLMSTP_PACKET Receive_Packet;
static DLMSTP_PACKET Transmit_Packet;
/* temp buffer for NPDU insertion */
/* local MS/TP port data - shared with RS-485 */
static volatile struct mstp_port_struct_t MSTP_Port;
/* buffers needed by mstp port struct */
static uint8_t TxBuffer[MAX_MPDU];
static uint8_t RxBuffer[MAX_MPDU];

#define INCREMENT_AND_LIMIT_UINT16(x) {if (x < 0xFFFF) x++;}

void dlmstp_millisecond_timer(void)
{
    INCREMENT_AND_LIMIT_UINT16(MSTP_Port.SilenceTimer);
}

void dlmstp_copy_bacnet_address(BACNET_ADDRESS * dest, BACNET_ADDRESS * src)
{
    int i = 0;

    if (src && dest) {
        src->mac_len = dest->mac_len;
        for (i = 0; i < MAX_MAC_LEN; i++) {
            src->mac[i] = dest->mac[i];
        }
        src->net = dest->net;
        src->len = dest->len;
        for (i = 0; i < MAX_MAC_LEN; i++) {
            src->adr[i] = dest->adr[i];
        }
    }
}

bool dlmstp_init(char *ifname)
{
    (void)ifname;
    /* initialize packet */
    Receive_Packet.ready = false;
    Receive_Packet.pdu_len = 0;
    /* initialize hardware */
    RS485_Initialize();
    /* initialize MS/TP data structures */
    MSTP_Port.InputBuffer = &RxBuffer[0];
    MSTP_Port.InputBufferSize = sizeof(RxBuffer);
    MSTP_Port.OutputBuffer = &TxBuffer[0];
    MSTP_Port.OutputBufferSize = sizeof(TxBuffer);
    MSTP_Init(&MSTP_Port);
    
    return true;
}

void dlmstp_cleanup(void)
{
    /* nothing to do for static buffers */
}

/* returns number of bytes sent on success, zero on failure */
int dlmstp_send_pdu(BACNET_ADDRESS * dest, /* destination address */
    BACNET_NPDU_DATA * npdu_data, /* network information */
    uint8_t * pdu, /* any data to be sent - may be null */
    unsigned pdu_len) /* number of bytes of data */
{
    int bytes_sent = 0;
    unsigned i = 0;             /* loop counter */

    if (Transmit_Packet.ready == false) {
        if (npdu_data->data_expecting_reply) {
            Transmit_Packet.frame_type = 
                FRAME_TYPE_BACNET_DATA_EXPECTING_REPLY;
        } else {
            Transmit_Packet.frame_type =
                FRAME_TYPE_BACNET_DATA_NOT_EXPECTING_REPLY;
        }
        Transmit_Packet.pdu_len = pdu_len;
        for (i = 0; i < pdu_len; i++) {
            Transmit_Packet.pdu[i] = pdu[i];
        }
        dlmstp_copy_bacnet_address(&Transmit_Packet.address, dest);
        bytes_sent = sizeof(Transmit_Packet);
    }

    return bytes_sent;
}

static void dlmstp_task(void)
{
    volatile AT91PS_PIO pPIO = AT91C_BASE_PIOA;
    /* only do receive state machine while we don't have a frame */
    if ((MSTP_Port.ReceivedValidFrame == false) &&
        (MSTP_Port.ReceivedInvalidFrame == false)) {
	/* LED OFF */
        pPIO->PIO_SODR = LED2;
        do {
            RS485_Check_UART_Data(&MSTP_Port);
            MSTP_Receive_Frame_FSM(&MSTP_Port);
            if (MSTP_Port.ReceivedValidFrame ||
                MSTP_Port.ReceivedInvalidFrame)
                break;
        } while (MSTP_Port.DataAvailable);
    } else {
	/* LED ON */
        pPIO->PIO_CODR = LED2;
    }
    /* only do master state machine while rx is idle */
    if (MSTP_Port.receive_state == MSTP_RECEIVE_STATE_IDLE) {
        while (MSTP_Master_Node_FSM(&MSTP_Port)) {
            /* do nothing */
        };
    }

    return;
}

/* copy the packet if one is received.
   Return the length of the packet */
uint16_t dlmstp_receive(
    BACNET_ADDRESS * src, /* source address */
    uint8_t * pdu, /* PDU data */
    uint16_t max_pdu, /* amount of space available in the PDU  */
    unsigned timeout) /* milliseconds to wait for a packet */
{
    unsigned i = 0; /* loop counter */
    uint16_t pdu_len = 0; /* return value */

    dlmstp_task();
    /* see if there is a packet available, and a place
       to put the reply (if necessary) and process it */
    if (Receive_Packet.ready) {
        if (Receive_Packet.pdu_len) {
            MSTP_Packets++;
            for (i = 0; i < Receive_Packet.pdu_len; i++) {
                pdu[i] = Receive_Packet.pdu[i];
            }
            dlmstp_copy_bacnet_address(src, &Receive_Packet.address);
            pdu_len = Receive_Packet.pdu_len;
        }
        Receive_Packet.ready = false;
    }

    return pdu_len;
}

void dlmstp_fill_bacnet_address(BACNET_ADDRESS * src, uint8_t mstp_address)
{
    int i = 0;

    if (mstp_address == MSTP_BROADCAST_ADDRESS) {
        /* mac_len = 0 if broadcast address */
        src->mac_len = 0;
        src->mac[0] = 0;
    } else {
        src->mac_len = 1;
        src->mac[0] = mstp_address;
    }
    /* fill with 0's starting with index 1; index 0 filled above */
    for (i = 1; i < MAX_MAC_LEN; i++) {
        src->mac[i] = 0;
    }
    src->net = 0;
    src->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        src->adr[i] = 0;
    }
}

/* for the MS/TP state machine to use for putting received data */
uint16_t MSTP_Put_Receive(
        volatile struct mstp_port_struct_t *mstp_port)
{
    DLMSTP_PACKET packet;
    uint16_t pdu_len = mstp_port->DataLength;
    unsigned i = 0;

    /* bounds check - maybe this should send an abort? */
    if (pdu_len > sizeof(packet.pdu))
        pdu_len = sizeof(packet.pdu);
    if (pdu_len) {
        MSTP_Packets++;
        for (i = 0; i < Receive_Packet.pdu_len; i++) {
            Receive_Packet.pdu[i] = mstp_port->InputBuffer[i];
        }
        dlmstp_fill_bacnet_address(&Receive_Packet.address,
            mstp_port->SourceAddress);
        Receive_Packet.pdu_len = pdu_len;
        Receive_Packet.ready = true; 
    }

    return pdu_len;
}

/* for the MS/TP state machine to use for getting data to send */
/* Return: amount of PDU data */
uint16_t MSTP_Get_Send(
    uint8_t src, /* source MS/TP address for creating packet */
    uint8_t * pdu, /* data to send */
    uint16_t max_pdu, /* amount of space available */
    unsigned timeout) /* milliseconds to wait for a packet */
{
    uint16_t pdu_len = 0; /* return value */
    uint8_t destination = 0;    /* destination address */

    if (Transmit_Packet.ready) {
        /* load destination MAC address */
        if (Transmit_Packet.address.mac_len == 1) {
            destination = Transmit_Packet.address.mac[0];
        } else {
            return 0;
        }
        if ((8 /* header len */  + Transmit_Packet.pdu_len) > MAX_MPDU) {
            return 0;
        }
        /* convert the PDU into the MSTP Frame */
        pdu_len = MSTP_Create_Frame(
            pdu, /* <-- loading this */
            max_pdu,
            Transmit_Packet.frame_type,
            destination, src,
            &Transmit_Packet.pdu[0],
            Transmit_Packet.pdu_len);
    }

    return pdu_len;
}

void dlmstp_set_mac_address(uint8_t mac_address)
{
    /* Master Nodes can only have address 0-127 */
    if (mac_address <= 127) {
        MSTP_Port.This_Station = mac_address;
        /* FIXME: implement your data storage */
        /* I2C_Write_Byte(
           EEPROM_DEVICE_ADDRESS, 
           mac_address,
           EEPROM_MSTP_MAC_ADDR); */
        if (mac_address > MSTP_Port.Nmax_master)
            dlmstp_set_max_master(mac_address);
    }

    return;
}

uint8_t dlmstp_mac_address(void)
{
    return MSTP_Port.This_Station;
}

/* This parameter represents the value of the Max_Info_Frames property of */
/* the node's Device object. The value of Max_Info_Frames specifies the */
/* maximum number of information frames the node may send before it must */
/* pass the token. Max_Info_Frames may have different values on different */
/* nodes. This may be used to allocate more or less of the available link */
/* bandwidth to particular nodes. If Max_Info_Frames is not writable in a */
/* node, its value shall be 1. */
void dlmstp_set_max_info_frames(uint8_t max_info_frames)
{
    if (max_info_frames >= 1) {
        MSTP_Port.Nmax_info_frames = max_info_frames;
        /* FIXME: implement your data storage */
        /* I2C_Write_Byte(  
           EEPROM_DEVICE_ADDRESS, 
           (uint8_t)max_info_frames,
           EEPROM_MSTP_MAX_INFO_FRAMES_ADDR); */
    }

    return;
}

uint8_t dlmstp_max_info_frames(void)
{
    return MSTP_Port.Nmax_info_frames;
}

/* This parameter represents the value of the Max_Master property of the */
/* node's Device object. The value of Max_Master specifies the highest */
/* allowable address for master nodes. The value of Max_Master shall be */
/* less than or equal to 127. If Max_Master is not writable in a node, */
/* its value shall be 127. */
void dlmstp_set_max_master(uint8_t max_master)
{
    if (max_master <= 127) {
        if (MSTP_Port.This_Station <= max_master) {
            MSTP_Port.Nmax_master = max_master;
            /* FIXME: implement your data storage */
            /* I2C_Write_Byte(
               EEPROM_DEVICE_ADDRESS, 
               max_master,
               EEPROM_MSTP_MAX_MASTER_ADDR); */
        }
    }

    return;
}

uint8_t dlmstp_max_master(void)
{
    return MSTP_Port.Nmax_master;
}

void dlmstp_get_my_address(BACNET_ADDRESS * my_address)
{
    int i = 0;                  /* counter */

    my_address->mac_len = 1;
    my_address->mac[0] = MSTP_Port.This_Station;
    my_address->net = 0;        /* local only, no routing */
    my_address->len = 0;
    for (i = 0; i < MAX_MAC_LEN; i++) {
        my_address->adr[i] = 0;
    }

    return;
}

void dlmstp_get_broadcast_address(BACNET_ADDRESS * dest)
{                               /* destination address */
    int i = 0;                  /* counter */

    if (dest) {
        dest->mac_len = 1;
        dest->mac[0] = MSTP_BROADCAST_ADDRESS;
        dest->net = BACNET_BROADCAST_NETWORK;
        dest->len = 0; /* always zero when DNET is broadcast */
        for (i = 0; i < MAX_MAC_LEN; i++) {
            dest->adr[i] = 0;
        }
    }

    return;
}