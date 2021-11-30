# Boiler_control

Обновление выполняется передачей бинки в модуль WIFI по протоколу tftp,
следующим образом:
* на хосте в директории с проектом, запустить tftp IP(client)
* написать команду определения типа передаваемого файла: binary
* написать команду отображения процесса передачи файла: trace
* написать команду передачи файла прошивки: put firmware/boiler.bin firmware.bin

Исправления в либе tftp сервера (файл прошивки свыше 260kB не принимался):
* находим и открываем файл ota-tftp.c
* находим метод tftp_receive_data
* находим проверку:

```c
if(len < DATA_PACKET_SZ)
{
const char *err = "Unknown validation error";
uint32_t image_length;
	if(!rboot_verify_image(start_offs, &image_length, &err) )
	{ //|| image_length != *received_len) { <---- убираем проверку длинны файла
		//printf("OTA TFTP: image_length = %d, received_len = %d\r\n", image_length, *received_len);
		tftp_send_error(nc, TFTP_ERR_ILLEGAL, err);
		return ERR_VAL;
	}
}
```