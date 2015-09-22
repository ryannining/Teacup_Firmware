
/** \file
  \brief I2C / TWI subsystem

  "TWI", "Two Wire Interface", is Atmel's name for the (patented) I2C bus
  system. I2C is technically pretty sophisticated, it also takes the current
  state of these two wires as part of the protocol into account. Like SPI it's
  a master/slave system with a clock signal on the wire. Unlike SPI,
  communication partners aren't choosen by setting a pin, but by by
  transferring an address byte before the actual data.

  Accordingly, code has to deal with states, transmissions have a start and
  an end, and actions on the bus can result in different states, like success
  or failure.

  avr-libc comes with good sample code:

    http://www.nongnu.org/avr-libc/examples/twitest/twitest.c

  For technical details see section 22 of atmega328 datasheet.
*/

#include "i2c.h"

#ifdef I2C

#include <avr/io.h>
#include <avr/interrupt.h>


#if defined I2C_MASTER_MODE && defined I2C_SLAVE_MODE
  #error Cant be I2C master and slave at the same time.
#endif


// Address of the device that is communicated with.
uint8_t i2c_address;
// State of TWI component of MCU.
volatile uint8_t i2c_state;
// Index into the send/receive buffer.
uint8_t i2c_index;
// Count of bytes to be sent.
uint8_t i2c_byte_count;

#ifdef I2C_EEPROM_SUPPORT
  // For SAWSARP mode (see ENHA in i2c.h).
  uint8_t i2c_page_address[I2C_PAGE_ADDRESS_SIZE];
  // Index into the page address buffer.
  uint8_t i2c_page_index;
  // Count of bytes in page address.
  uint8_t i2c_page_count;
#endif /* I2C_EEPROM_SUPPORT */

#ifdef I2C_SLAVE_MODE
  uint8_t i2c_in_buffer[I2C_SLAVE_RX_BUFFER_SIZE];
  uint8_t i2c_out_buffer[I2C_SLAVE_TX_BUFFER_SIZE];
#else
  uint8_t *i2c_buffer;
#endif /* I2C_SLAVE_MODE */

I2C_HANDLER i2c_master_func = &i2c_do_nothing;
I2C_HANDLER i2c_slave_func = &i2c_do_nothing;
I2C_HANDLER i2c_error_func = &i2c_do_nothing;


void i2c_init(uint8_t address, I2C_HANDLER func) {

  i2c_address = address;

  #ifdef I2C_MASTER_MODE
    #ifdef I2C_ENABLE_PULLUPS
      I2C_DDR &= ~((1 << I2C_SCL_PIN) | (1 << I2C_SDA_PIN));
      I2C_PORT |= (1 << I2C_SCL_PIN) | (1 << I2C_SDA_PIN);
    #endif /* I2C_ENABLE_PULLUPS */

    i2c_master_func = func;

    /**
      TWI Bit Rate register
      SCL_freq = CPU_freq / (16 + 2 * TWBR)
      See: page 235 of atmega328 datasheet.
    */
    TWBR = ((F_CPU / I2C_BITRATE) - 16) / 2;

    /**
      TWI Status Register
      Lower two bits set the prescaler value.
      See: page 236 of atmega328 datasheet.
    */
    TWSR = 0x00;
  #endif /* I2C_MASTER_MODE */

  #ifdef I2C_SLAVE_MODE
    i2c_slave_func = func;
    TWAR = i2c_address; // We listen to broadcasts if lowest bit is set.
    TWCR = (0<<TWINT)|(0<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
  #endif
}

static void i2c_send_handler(void) {

  i2c_state = I2C_MODE_SAWP; // Just send.
  i2c_master_func = &i2c_send_handler;
  i2c_error_func = &i2c_send_handler;

  // Start transmission.
  TWCR = (1<<TWINT)|(0<<TWEA)|(1<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
  i2c_state |= I2C_MODE_BUSY;
}

/**
  Function sends a data block to slave device.
*/
void i2c_send(uint8_t address, uint8_t* block, uint8_t tx_len) {

  i2c_address = address;
  i2c_buffer = block;
  i2c_index = 0;
  i2c_byte_count = tx_len;

  i2c_send_handler();
}

/**
  Empty handler.
*/
void i2c_do_nothing(void) {
}

/**
  This is the interrupt for all the I2C things.

  A few words about TWCR bits:

    Bit 7 (TWINT) is used to run TWI.

    Bit 6 (TWEA) is used to send ACK (if set) in cases of

      a) device's own slave address has been received;
      b) general call has been received;
      c) a data byte has been received.

    Bit 5 (TWSTA) is 1 if app wants to be a master, don't forget to
                  clear this bit.

    Bit 4 (TWSTO) is generated STOP in master mode if set (cleared
                  automaticly), recovered from error condition in slave
                  mode if set.

    Bit 3 (TWWC) is write collision flag. Sets on try to writeto TWDR when
                 TWINT is low.

    Bit 2 (TWEN) activates SDA/SCL pins if set. Set to 0 to disable TWI.

    Bit 1 (Reserved).

    Bit 0 (TWIE) enables TWI interrupt.
*/
ISR(TWI_vect) {

  switch (TWSR & 0xF8) {  // Cut the prescaler bits out.
    case I2C_STATE_BUS_FAIL:
      // A hardware error was detected.
      i2c_state |= I2C_ERROR_BUS_FAIL;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      MACRO_I2C_ERROR;
      break;
    case I2C_STATE_START:
      // Start happens, send a target address.
      if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_SARP) {
        i2c_address |= 0x01;
      } else {
        i2c_address &= 0xFE;
      }
      TWDR = i2c_address;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      break;
    case I2C_STATE_RESTART:
      // Start happens, send a target address.
      if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_ENHA) {
        i2c_address |= 0x01;
      } else {
        i2c_address &= 0xFE;
      }
      TWDR = i2c_address;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      break;
    case I2C_STATE_SLAWACK:
      // SLA+W was sent, then ACK received.
      if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_SAWP) {
        TWDR = i2c_buffer[i2c_index++];
        TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      #ifdef I2C_EEPROM_SUPPORT
        if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_ENHA) {
          TWDR = i2c_page_address[i2c_page_index++];
          TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
        }
      #endif
      break;
    case I2C_STATE_SLAWNACK:
      // SLA+W was sent, got NACK, so slave is busy or out of bus.
      i2c_state |= I2C_ERROR_NO_ANSWER;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      MACRO_I2C_ERROR;
      break;
    case I2C_STATE_BYTEACK:
      // A byte was sent, got ACK.
      if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_SAWP) {
        if (i2c_index == i2c_byte_count) {
          // Last byte, send stop condition.
          TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(0<<TWIE);
        } else {
          // Send the next byte.
          TWDR = i2c_buffer[i2c_index++];
          TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
        }
      }
      #ifdef I2C_EEPROM_SUPPORT
        if ((i2c_state & I2C_MODE_MASK) == I2C_MODE_ENHA) {
          // It was a page address byte.
          if (i2c_page_index == i2c_page_count) {
            // It was the last byte of the page address.
            TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(1<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
          } else {
            // Send the next page address byte.
            TWDR = i2c_page_address[i2c_page_index++];
            TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
          }
        }
      #endif
      break;
    case I2C_STATE_BYTENACK:
      // Byte was sent but got NACK, there are two possible reasons:
      //  - a slave stops transmission and it is ok, or
      //  - a slave became crazy.
      i2c_state |= I2C_ERROR_NACK;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      MACRO_I2C_MASTER; // process exit state
      break;
    case I2C_STATE_COLLISION:
      // It looks like there is another master on the bus.
      i2c_state |= I2C_ERROR_LOW_PRIO;
      // Setup all again.
      i2c_index = 0;
      #ifdef I2C_EEPROM_SUPPORT
        i2c_page_index = 0;
      #endif
      // Try to resend when the bus became free.
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(1<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      break;
    case I2C_STATE_SLARACK:
      // SLA+R was sent, got АСК, then received a byte.
      if (i2c_index + 1 == i2c_byte_count) {
        // Last byte fitting into the buffer. Request a byte, then send NACK
        // to slave and it will release the bus.
        TWCR = (1<<TWINT)|(0<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Or just receive a byte and sent ACK.
        TWCR = (1<<TWINT)|(1<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_SLARNACK:
      // SLA+R was sent, got NАСК, it seems the slave is busy.
      i2c_state |= I2C_ERROR_NO_ANSWER;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      MACRO_I2C_ERROR;
      break;
    case I2C_STATE_GOT_BYTE:
      i2c_buffer[i2c_index++] = TWDR;
      // TODO: Add BUFFER OVERFLOW check.
      if (i2c_index + 1 == i2c_byte_count) {
        // Last byte wait the processing.
        TWCR = (1<<TWINT)|(0<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Request the next byte.
        TWCR = (1<<TWINT)|(0<<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_GOT_BYTE_NACK:
      // Last byte received, send NACK to make the slave to release the bus.
      i2c_buffer[i2c_index] = TWDR;
      TWCR = (1<<TWINT)|(I2C_MODE<<TWEA)|(0<<TWSTA)|(1<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      MACRO_I2C_MASTER;
      break;
    case I2C_STATE_SLAW_LP:
    case I2C_STATE_SLAW_LP_ANY:
      // Another master on the bus sent some bytes, receive them.
      i2c_state |= I2C_ERROR_LOW_PRIO;
      // Restore the transfer.
      i2c_index = 0;
      #ifdef I2C_EEPROM_SUPPORT
        i2c_page_index = 0;
      #endif

    #ifdef I2C_SLAVE_MODE

    case I2C_STATE_SLAW:
    case I2C_STATE_SLAW_ANY:
      i2c_state |= I2C_MODE_BUSY; // Lock bus.
      i2c_index = 0;
      if (I2C_SLAVE_RX_BUFFER_SIZE == 1) {
        // We should take alone byte and send NACK.
        TWCR = (1<<TWINT)|(0<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Get a byte and send ACK.
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_RCV_BYTE:
    case I2C_STATE_RCV_BYTE_ANY:
      i2c_in_buffer[i2c_index++] = TWDR;
      if (i2c_index == I2C_SLAVE_RX_BUFFER_SIZE - 1) {
        // Room for only one byte left, send NACK.
        TWCR = (1<<TWINT)|(0<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // We have room for more bytes, send ACK.
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_RCV_LAST_BYTE:
    case I2C_STATE_RCV_LAST_BYTE_ANY:
      i2c_in_buffer[i2c_index] = TWDR;
      if (i2c_state & I2C_INTERRUPTED) {
        // Если у нас был прерываный сеанс от имени мастера
        // Влепим в шину свой Start поскорей и сделаем еще одну попытку
        TWCR = (1<<TWINT)|(1<TWEA)|(1<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Если не было такого факта, то просто отвалимся и будем ждать
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      MACRO_I2C_SLAVE;
      break;
    case I2C_STATE_RCV_RESTART:
      // We got a Restart. What we will do?
      // Here we can do additional logic but we don't need it at this time.
      // Just ignore it now.
      TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      break;
    case I2C_STATE_RCV_SLAR_LP:
      // Got own address on read from another master.
      i2c_state |= I2C_ERROR_LOW_PRIO | I2C_INTERRUPTED;

      // Reinit.
      i2c_index = 0;
      #ifdef I2C_EEPROM_SUPPORT
        i2c_page_index = 0;
      #endif
    case I2C_STATE_RCV_SLAR:
      // We have got own address on read.
      i2c_index = 0;
      TWDR = i2c_out_buffer[i2c_index];
      if (I2C_SLAVE_TX_BUFFER_SIZE == 1) {
        // If it is last byte, we hope to receive NACK.
        TWCR = (1<<TWINT)|(0<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Wait for ACK.
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_SND_BYTE_ACK:
      // Send byte and got ACK, then send next byte to master.
      TWDR = i2c_out_buffer[++i2c_index];
      if (I2C_SLAVE_TX_BUFFER_SIZE - 1 == i2c_index) {
        // It was last byte, send it and wait for NACK.
        TWCR = (1<<TWINT)|(0<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // Send byte and wait for ACK.
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(0<<TWEN)|(1<<TWIE);
      }
      break;
    case I2C_STATE_SND_LAST_BYTE_NACK:
    case I2C_STATE_SND_LAST_BYTE_ACK:
      // We sent the last byte and received NACK or ACK (doesn't matter here).
      if (i2c_state & I2C_INTERRUPTED) {
        // There was interrupted master transfer.
        i2c_state &= I2C_NOINTERRUPTED;
        // Generate start as the bus became free.
        TWCR = (1<<TWINT)|(1<TWEA)|(1<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      } else {
        // If we alone then just release the bus.
        TWCR = (1<<TWINT)|(1<TWEA)|(0<<TWSTA)|(0<<TWSTO)|(1<<TWEN)|(1<<TWIE);
      }
      MACRO_I2C_SLAVE;
      break;

    #endif /* I2C_SLAVE_MODE */

    default:
      break;
    }
}

#endif /* I2C */