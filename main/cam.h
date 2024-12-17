#ifndef CAM_H
#define CAM_H

#include <esp_err.h>
#include <esp_camera.h>

#define CAM_PIN_PWDN  -1   // should be 10 / A0 
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK   2   //XCLK   x
#define CAM_PIN_SIOD  35   //SDA    x
#define CAM_PIN_SIOC  36   //SCL    x

#define CAM_PIN_D7    45   // (D9)  x
#define CAM_PIN_D6    12   // (D8)  x
#define CAM_PIN_D5    17   // (D7)  x
#define CAM_PIN_D4    18   // (D6)  x   
#define CAM_PIN_D3    15   // (D5)  x
#define CAM_PIN_D2    16   // (D4)  x
#define CAM_PIN_D1    37   // (D3)  x
#define CAM_PIN_D0     6   // (D2)  x
#define CAM_PIN_VSYNC  3   //VSYNC  x
#define CAM_PIN_HREF   8   //HS     x
#define CAM_PIN_PCLK   1   //PC     x

esp_err_t init_cam(void);

#endif