# Diagnóstico USB BLE opcional

Compilar con `pio run -e lilygo_lora32_ble_debug`, o añadir `-D RFLINK_BLE_DEBUG` a un entorno ESP32 que ya tenga BLE habilitado. La opción de configuración `ble.enabled` sigue siendo necesaria. Abrir USB a 115200 baudios. El entorno normal `lilygo_lora32` no incluye el capturador, sus contadores, su tarea ni sus trazas. El commit de diagnóstico puede revertirse completo sin retirar el [protocolo de disponibilidad](BLE_Transport.md).

Los callbacks solo capturan registros de tamaño fijo, sin imprimir, esperar ni reservar memoria para el diagnóstico. Una tarea independiente, de prioridad 1, envía como máximo **128 bytes cada 100 ms** y produce un resumen aproximadamente cada **2 segundos**. Así puede mostrar la última ejecución del consumidor aunque este no vuelva a `mainLoop`. La cola de trazas tiene **48 registros**; al llenarse aumenta `lost`, sin bloquear ni alterar la cola de comandos. Los contadores continúan aunque se pierdan trazas. No se registran payloads ni claves; el PIN conserva exclusivamente su salida USB habitual. La salida puede intercalarse con otros mensajes USB del firmware.

Cada evento `[BLEDBG]` incluye:

- `t`: timestamp de captura, `millis()` desde arranque (uint32, wrap habitual).
- `h`: connection handle; `65535` indica ninguno. `g`: generación actual, incrementada en cada conexión, incluso si se reutiliza `h`. Los eventos de otro handle dicen `ignored`; su `g` identifica el estado actual que no se modificó.
- Evento: `connect`, `authentication`, `tx_subscribe`, `status_subscribe`, `disconnect`, `write_enter`, `write_result`, `queue_reset`, `availability`, `status_read`, `status_notify`, `status_completion` o fallo `security_start`.
- `n`: bytes de escritura, reinicio de cola o notificación. `result`: aceptado/rechazado con motivo. `detail`: código de desconexión/completado, valor CCCD (`0` desuscrito, `1` NOTIFY) o reason de disponibilidad, según evento.
- `enc`, `auth`, `bond`, `key`: cifrado, autenticación, bonding y tamaño de clave en bytes, obtenidos del `NimBLEConnInfo` de ese callback. Los eventos internos usan la última muestra de la conexión actual. `status_completion` no recibe identidad del peer en NimBLE y lo indica explícitamente; solo solicita releer/notificar estado actual.

Motivos de `write_result`: `accepted`, `wrong_connection`, `not_encrypted`, `not_authenticated`, `not_bonded`, `key_size`, `auth_callback_pending`, `consumer_pending`, `cleanup_pending`, `tx_unsubscribed`, `queue_overflow`. La entrada y el resultado se capturan por separado; el contador `write` se incrementa al entrar, antes de obtener el valor del atributo.

El resumen usa contadores acumulados desde el arranque (uint32):

| Campo | Significado |
| --- | --- |
| write / ok / reject | Entradas en onWrite / escrituras aceptadas / rechazadas |
| enq | Bytes aceptados y encolados |
| drop | Bytes de escrituras rechazadas más bytes retirados por reinicios de cola |
| used / q | Bytes extraídos por el consumidor / bytes actualmente en cola |
| reset / reset_bytes | Reinicios de cola / bytes descartados en esos reinicios (incluidos en drop) |
| cmd | Órdenes entregadas al flujo de ejecución |
| partial | Bytes acumulados en una línea aún sin terminar |
| parser_drop / discard_line | Bytes consumidos que el parser descartó / ignorando una línea demasiado larga |
| last_rx / last_cons / age | Última entrada en onWrite / última entrada al consumidor / ms desde esta última |
| reason | Motivo de disponibilidad, según BLE_Transport.md |
| nf | Fallos inmediatos o asíncronos del envío de estado (también disponibles en JSON sin debug) |
| lost | Eventos de diagnóstico no guardados por falta de espacio |

`used` incluye bytes del parser que después pueden ser descartados; `parser_drop` es una clasificación de esos bytes, no debe sumarse como si fueran nuevas entradas. Los resúmenes son muestras operativas entre tareas, no una transacción conjunta de todos los contadores.

Para localizar la pérdida, enviar unos pocos `10;PING;\n` separados y comparar resúmenes:

1. **ACK GATT pero write/last_rx no cambian**: no se entró en `onWrite` de RX. Revisar atributo/servicio realmente escritos, caché GATT y traza ATT/CoreBluetooth. La captura no permite afirmar qué ocurrió antes del callback; no confundir ACK con ejecución RFLink.
2. **write crece, reject crece**: `write_result` y los cuatro campos de seguridad explican el rechazo. `lost>0` avisa de que puede faltar el evento individual; repetir a menor frecuencia.
3. **ok/enq crecen, used no crece y q queda pendiente**: escritura aceptada sin consumo. `last_cons/age` distingue un consumidor detenido/lento; `reason`, seguridad y suscripción explican si se ha suspendido el consumo.
4. **used crece pero cmd no**: revisar `partial`, terminador CR/LF, `parser_drop` y `discard_line`. Un `10;PING;` sin salto de línea permanece parcial legítimamente.
5. **reset/reset_bytes crecen**: la cola se limpió; correlacionar con handle, generación y desconexión. **status_notify failed** o **status_completion failed_no_peer_id**: releer estado; el fallo no convierte por sí mismo READY en falso.

El capturador tiene un coste limitado, pero no es una medida exacta de tiempos de radio. Para una prueba sin su coste, usar el entorno normal o revertir el commit de debug. La validación de la app y los fallos de radio requiere hardware BLE real.

## Inventario GATT al arrancar

El firmware de debug imprime además **tres líneas fijas al arrancar**, una por RX, TX y estado, después de registrar los servicios y arrancar el anuncio. Este informe de arranque se imprime desde setup antes de arrancar la tarea de resúmenes, para evitar que esta intercale sus líneas; no forma parte del presupuesto periódico de 128 bytes/100 ms ni se ejecuta en callbacks.

Cada línea `[BLEDBG] ... gatt` consulta la tabla del stack mediante `ble_gatts_find_svc` y `ble_gatts_find_chr`. `svc_rc=0` y `chr_rc=0`, con handles no nulos, confirman que esa característica está registrada bajo ese servicio. `decl_h` es el handle de declaración y `value_h` el de valor; no son connection handles. `props` son flags internos NimBLE, incluidos los de seguridad, no el valor bruto de CBCharacteristicProperties.

Si el iPhone devuelve una lista vacía para el servicio de estado, comparar ese resultado con la línea del UUID `A8F10002-8D5B-4A6D-9F32-70E4B2C6D901`. Un lookup correcto en firmware y una lista vacía en iOS acotan la investigación a descubrimiento, caché o la respuesta ATT recibida; no prueban por sí solos un fallo de caché. Véase [recuperación y diagnóstico CoreBluetooth](BLE_iOS_Discovery.md).
