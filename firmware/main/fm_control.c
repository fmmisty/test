#include "fm_control.h"
#include <math.h>
#include <string.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
static const char *TAG="fmctl"; static spi_device_handle_t s_fpga,s_adf; static fm_status_t s={.frequency_khz=92000,.kv_centi=1500,.comp_en=true};
static uint8_t calc_kidx(uint16_t n,uint16_t kv){double lo=10.0/1080.0,hi=20.0/760.0,r=(kv/100.0)/n;long x=lround(127.0*log(r/lo)/log(hi/lo));return x<0?0:x>127?127:(uint8_t)x;}
static esp_err_t fpga_xfer(uint16_t tx,uint16_t *rx){gpio_set_level(BOARD_ADF_LE,0);spi_transaction_t t={.length=16,.tx_buffer=&tx,.rx_buffer=rx};return spi_device_transmit(s_fpga,&t);}
static esp_err_t fpga_write(uint8_t a,uint16_t d){uint16_t tx=__builtin_bswap16(((a&7)<<12)|(d&0xfff)),rx=0;return fpga_xfer(tx,&rx);}
static esp_err_t adf_write(uint32_t w){uint8_t b[3]={w>>16,w>>8,w};spi_transaction_t t={.length=24,.tx_buffer=b};gpio_set_level(BOARD_ADF_LE,0);ESP_RETURN_ON_ERROR(spi_device_transmit(s_adf,&t),TAG,"ADF SPI");gpio_set_level(BOARD_ADF_LE,1);esp_rom_delay_us(2);gpio_set_level(BOARD_ADF_LE,0);return ESP_OK;}
static uint32_t adf_func(uint8_t c){return(3u<<18)|(1u<<7)|(1u<<4)|c;} static uint32_t adf_n(uint16_t n,bool acq){return((uint32_t)acq<<21)|((uint32_t)n<<8)|1;}
static void safety_task(void *arg){(void)arg;for(;;){if(s.rf_on&&!gpio_get_level(BOARD_ADF_MUXOUT)){gpio_set_level(BOARD_FM_TX_EN,0);fpga_write(0,2|(s.comp_en?1:0));s.rf_on=false;s.pll_lock=false;}vTaskDelay(pdMS_TO_TICKS(10));}}
esp_err_t fm_control_init(void){gpio_config_t o={.pin_bit_mask=(1ULL<<BOARD_ADF_LE)|(1ULL<<BOARD_FM_TX_EN)|(1ULL<<BOARD_FPGA_RST),.mode=GPIO_MODE_OUTPUT};ESP_ERROR_CHECK(gpio_config(&o));gpio_set_level(BOARD_ADF_LE,0);gpio_set_level(BOARD_FM_TX_EN,0);gpio_set_level(BOARD_FPGA_RST,1);gpio_config_t i={.pin_bit_mask=1ULL<<BOARD_ADF_MUXOUT,.mode=GPIO_MODE_INPUT,.pull_down_en=1};ESP_ERROR_CHECK(gpio_config(&i));spi_bus_config_t bus={.mosi_io_num=BOARD_SPI_MOSI,.miso_io_num=BOARD_SPI_MISO,.sclk_io_num=BOARD_SPI_SCK,.quadwp_io_num=-1,.quadhd_io_num=-1};ESP_ERROR_CHECK(spi_bus_initialize(BOARD_SPI_HOST,&bus,SPI_DMA_CH_AUTO));spi_device_interface_config_t f={.clock_speed_hz=1000000,.mode=0,.spics_io_num=BOARD_FPGA_CS,.queue_size=1};ESP_ERROR_CHECK(spi_bus_add_device(BOARD_SPI_HOST,&f,&s_fpga));spi_device_interface_config_t a={.clock_speed_hz=1000000,.mode=0,.spics_io_num=-1,.queue_size=1};ESP_ERROR_CHECK(spi_bus_add_device(BOARD_SPI_HOST,&a,&s_adf));nvs_handle_t h;if(nvs_open("fmctl",NVS_READONLY,&h)==ESP_OK){size_t z=sizeof(s);nvs_get_blob(h,"settings",&s,&z);nvs_close(h);}s.rf_on=false;fpga_write(0,2);xTaskCreate(safety_task,"fm_safe",3072,NULL,8,NULL);return ESP_OK;}
esp_err_t fm_control_set(uint32_t khz,uint16_t kv,bool comp){ESP_RETURN_ON_FALSE(khz>=76000&&khz<=108000&&khz%100==0,ESP_ERR_INVALID_ARG,TAG,"frequency");ESP_RETURN_ON_FALSE(kv>=1000&&kv<=2000,ESP_ERR_INVALID_ARG,TAG,"Kv");uint16_t n=khz/100;gpio_set_level(BOARD_FM_TX_EN,0);s.rf_on=false;ESP_RETURN_ON_ERROR(fpga_write(0,2),TAG,"mute");ESP_RETURN_ON_ERROR(adf_write(adf_func(3)),TAG,"init");ESP_RETURN_ON_ERROR(adf_write(adf_func(2)),TAG,"function");ESP_RETURN_ON_ERROR(adf_write((2u<<16)|(100u<<2)),TAG,"R");ESP_RETURN_ON_ERROR(adf_write(adf_n(n,true)),TAG,"N");for(int i=0;i<100&&!gpio_get_level(BOARD_ADF_MUXOUT);i++)vTaskDelay(pdMS_TO_TICKS(10));ESP_RETURN_ON_FALSE(gpio_get_level(BOARD_ADF_MUXOUT),ESP_ERR_TIMEOUT,TAG,"PLL lock");s.frequency_khz=khz;s.kv_centi=kv;s.kidx=calc_kidx(n,kv);s.comp_en=comp;s.pll_lock=true;ESP_RETURN_ON_ERROR(fpga_write(2,n),TAG,"NCH");ESP_RETURN_ON_ERROR(fpga_write(3,kv),TAG,"KVCAL");ESP_RETURN_ON_ERROR(fpga_write(1,s.kidx),TAG,"KIDX");ESP_RETURN_ON_ERROR(fpga_write(0,2|(comp?1:0)),TAG,"CTRL");ESP_RETURN_ON_ERROR(adf_write(adf_n(n,false)),TAG,"TX current");return ESP_OK;}
esp_err_t fm_control_set_rf(bool on){
 if(!on){gpio_set_level(BOARD_FM_TX_EN,0);fpga_write(0,2|(s.comp_en?1:0));s.rf_on=false;return ESP_OK;}
#if CONFIG_FM_ALLOW_RF_OUTPUT
 ESP_RETURN_ON_FALSE(gpio_get_level(BOARD_ADF_MUXOUT),ESP_ERR_INVALID_STATE,TAG,"PLL unlock");fpga_write(0,s.comp_en?1:0);gpio_set_level(BOARD_FM_TX_EN,1);s.rf_on=true;return ESP_OK;
#else
 return ESP_ERR_NOT_SUPPORTED;
#endif
}
void fm_control_get(fm_status_t*out){s.pll_lock=gpio_get_level(BOARD_ADF_MUXOUT);if(!s.pll_lock&&s.rf_on)fm_control_set_rf(false);*out=s;}
esp_err_t fm_control_save(void){nvs_handle_t h;ESP_RETURN_ON_ERROR(nvs_open("fmctl",NVS_READWRITE,&h),TAG,"nvs");fm_status_t t=s;t.rf_on=false;esp_err_t e=nvs_set_blob(h,"settings",&t,sizeof(t));if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);return e;}
