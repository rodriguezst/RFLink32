# Transporte BLE RFLink32 — versión 1

Nordic UART conserva el servicio `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, RX `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` (WRITE/WRITE_NR) y TX `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` (NOTIFY). No cambia el formato RFLink: por ejemplo `10;PING;\n` responde con el `PONG` habitual. Un ACK GATT confirma la escritura del atributo, **no** su aceptación ni ejecución por RFLink. Los comandos necesitan CR, LF o CRLF; el punto y coma final no sustituye al salto de línea.

El estado se descubre en un **servicio adicional**, sin modificar ni ampliar Nordic UART:

- Servicio: `A8F10001-8D5B-4A6D-9F32-70E4B2C6D901`.
- Característica: `A8F10002-8D5B-4A6D-9F32-70E4B2C6D901`, **READ + NOTIFY**.
- El anuncio sigue incluyendo Nordic UART; descubrir el servicio adicional tras conectar.
- READ exige cifrado y autenticación ATT. Lecturas y notificaciones de estado requieren también bonding y clave de 16 bytes, como los comandos. Se conserva Secure Connections con MITM y PIN por USB. Una lectura durante una seguridad insuficiente puede fallar en ATT o devolver valor vacío; nunca debe interpretarse como READY.

Valor binario de **17 bytes**, sin padding, todos los enteros sin signo en **little endian** (cabe en el MTU mínimo):

| Offset | Bytes | Campo | Significado |
| --- | --- | --- | --- |
| 0 | 1 | version | `1` |
| 1 | 8 | boot_id | Aleatorio de 64 bits generado al arrancar; comparar como identificador opaco |
| 9 | 4 | session_id | Generación incremental por conexión; `0` significa desconectado |
| 13 | 1 | ready | `0` o `1` |
| 14 | 1 | reason | Tabla siguiente; `0` solo cuando ready es `1` |
| 15 | 2 | max_command_bytes | `INPUT_COMMAND_SIZE - 1`, actualmente **1999**; **excluye CR/LF/CRLF** y el NUL interno |

| reason | Significado |
| --- | --- |
| 0 | Preparado |
| 1 | Desconectado |
| 2 | Seguridad pendiente o insuficiente |
| 3 | Consumidor pendiente de inicialización/publicación |
| 4 | Limpieza de la sesión anterior pendiente |
| 5 | Falta suscripción a notificaciones TX de Nordic UART |
| 6 | Desbordamiento: escritura rechazada completa; se cerrará la conexión |
| 7 | Cola llena temporalmente: esperar al consumidor |

`ready=1` requiere seguridad completa de la conexión actual, cola y consumidor inicializados, limpieza terminada y suscripción TX activa. Solo el flujo consumidor lo publica. La suscripción al estado es opcional: los clientes Nordic UART anteriores siguen funcionando después de autenticar y suscribirse a TX. Los callbacks de autenticación/desconexión/suscripción comprueban el handle actual. NimBLE serializa estos callbacks en su tarea host; el trabajo diferido usa el estado actual, y el parser verifica la generación para no mezclar conexiones aunque se reutilice un handle. La desconexión invalida inmediatamente READY y session_id; la publicidad se reanuda después de limpiar en el consumidor.

Secuencia de la app:

1. Conectar, descubrir ambos servicios y completar emparejamiento/autenticación (PIN mostrado solo por USB si corresponde).
2. Activar NOTIFY en TX Nordic UART. Opcionalmente activar NOTIFY en estado; autenticación y suscripciones pueden completarse en cualquier orden.
3. Leer estado después de suscribirse. La suscripción al estado también solicita su valor actual cuando la seguridad lo permita, incluso si ocurrió antes de autenticarse. Aceptar solo formato conocido de 17 bytes y guardar `(boot_id, session_id)` asociado a esta conexión local.
4. Esperar `ready=1` antes de escribir. Enviar, por ejemplo, los bytes UTF-8 de `10;PING;\n`; esperar `PONG` por TX. Fragmentar según el límite de escritura de CoreBluetooth/MTU, sin superar 512 bytes por escritura ni el límite total del comando. Para evitar desbordamientos, enviar un comando y esperar su respuesta antes del siguiente.
5. Los cambios de estado solicitan notificación (los cambios transitorios pueden agruparse). Si no llega, releer: una notificación no es una confirmación fiable de entrega a la app. Si el envío falla, el estado sigue disponible para lectura, aumenta `status_failed_notifications` en el estado JSON y se reintenta desde el consumidor con intervalo mínimo de 250 ms.
6. Con `ready=0`, detener nuevos comandos. Si la cola se desborda, se rechaza toda la escritura y se desconecta para no ejecutar una línea con fragmentos perdidos; reconectar y repetir la secuencia. Una cola simplemente llena puede drenarse sin desconexión. No reintentar automáticamente comandos con efectos si su ejecución es incierta.
7. Al desconectar, descartar inmediatamente la disponibilidad y las operaciones pendientes de esa conexión. En la siguiente conexión obtener un nuevo estado; no reutilizar READY de la anterior. Una orden ya entregada al ejecutor antes de desconectar no puede deshacerse.

READY no es un crédito de cola ni una confirmación por comando: otras escrituras pueden llenar la cola después de leerlo. La aceptación usa los mismos criterios, más espacio suficiente para la escritura completa. Las líneas demasiado largas conservan el error RFLink existente y se descartan hasta CR/LF.

Pruebas de host: `g++ -std=c++11 -Wall -Wextra -Werror tests/ble_transport_state_test.cpp -o /tmp/ble_transport_test && /tmp/ble_transport_test`. Compilación ESP32: `pio run -e lilygo_lora32`.

Validación con dispositivo/app: primer emparejamiento y reconexión bonded; ambos órdenes auth/TX; estado antes/después de auth; cliente sin característica de estado; desuscribir/resuscribir TX; reconectar durante un comando fragmentado; clave/seguridad insuficientes; cola saturada; pérdida de notificación y recuperación por READ; reinicio con nuevo boot_id. Al actualizar firmware, volver a descubrir servicios si el sistema mantiene una caché GATT anterior.

### Caché GATT tras reiniciar o actualizar el firmware

Antes de anunciarse, el firmware registra toda la tabla GATT y llama a la API estándar Service Changed de NimBLE para el rango `0x0001..0xffff`. Se hace **una vez por arranque**, de forma conservadora, porque el aviso automático de NimBLE solo cubre cambios realizados después de arrancar el servidor. Los UUID, los requisitos de seguridad y el formato del estado permanecen iguales.

NimBLE conserva el aviso pendiente para los clientes bonded suscritos a Service Changed (`1801/2A05`) y lo entrega al restaurar su cifrado, bond y CCCD. No se borra ningún bond ni se fuerza una nueva vinculación. El aviso se confirma mediante una indicación ATT; no se vuelve a marcar la tabla al reconectar dentro del mismo arranque. Si se reinicia antes de entregar un aviso, la llamada de arranque también restablece el rango de handles, que NimBLE guarda en RAM.

En iOS, `peripheral(_:didModifyServices:)` debe invalidar READY y las referencias GATT afectadas. Antes de escribir RF, la app puede volver a descubrir ambos servicios dentro del plazo original; después de empezar a escribir, debe terminar con resultado incierto, sin reintento. Cancelar el intento también es seguro, pero puede requerir que el usuario lo repita una vez después de reiniciar el puente. Véase el [ejemplo de recuperación CoreBluetooth](BLE_iOS_Discovery.md#recover-service-changed-before-sending).

Service Changed solo puede invalidar por esta vía a clientes que tengan registrada la suscripción estándar. Si el resultado sigue vacío, comprobar el inventario GATT de arranque y la recepción de la indicación/`didModifyServices`; no dar por demostrada una caché defectuosa ni sustituir el protocolo READY por PING. En hardware, verificar actualización con bond existente, reinicio con indicación pendiente, y dos conexiones consecutivas sin reiniciar: la segunda no debe producir un nuevo aviso provocado por el firmware si el primero ya fue confirmado.
