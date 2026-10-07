# Optimización del player nativo de Blender y Houdini

3 de octubre de 2026. Cambios integrados en el código compartido por ambos plugins,
compilados con MSVC Release. No cambia el ABI 4, los archivos VGS/PGS, el trainer,
las opciones de densidad, los valores de los frames ni los shaders.

## Implementación

`plugins/vgsblender/src/framepack.h` contiene la conversión privada de un frame del
decoder al formato de atributos de los hosts. `Slot` conserva esos buffers y
`decodeInto()` utiliza la conversión. Houdini compila el mismo player, de modo que
recibe ambos cambios al reconstruir su DSO:

- A densidad completa, se utiliza `Frame::activeCount`, ya calculado por el decoder,
  en lugar de recorrer otra vez todos los registros para contar los vivos. A densidad
  reducida se mantiene el recuento de la selección por hash.
- Se conserva la longitud inicializada máxima de los buffers reutilizados. Al disminuir
  y volver a aumentar el número de splats, no se ponen a cero floats que inmediatamente
  se sobrescribirán. Los campos `count` y `sh_coefficients` siguen delimitando lo que
  puede leer el host. La salida SH se publica como nula cuando está desactivada o vacía.

No se retienen vistas del decoder ni se añaden buffers de índices de tamaño frame.
Los slots mantienen memoria propia y su protección mientras el host los utiliza.
Las capacidades de los vectores ya se conservaban antes; este cambio conserva también
su longitud inicializada. No se afirma una reducción del pico de RAM.

Se descartó la reorganización SH por bloques: la primera prueba empeoró su conversión
en boxing. Se conserva el recorrido SH original y sus mismas operaciones numéricas.
Blender ya usa vistas NumPy de la memoria nativa; `foreach_set()` copia a los atributos
que pertenecen a Blender. El SOP de Houdini escribe sus propios atributos. Esas copias
de entrega y las transformaciones del host se mantienen.

## Medición

Se guardó una DLL anterior compilada con el source presente al empezar esta tarea
(incluidas las modificaciones previas del usuario), y se comparó con la nueva DLL
usando la misma configuración. No es una comparación contra un binario antiguo de
otra versión. Benchmarks ejecutados secuencialmente en Ryzen 9 7900X, Windows.

Conversión aislada de frames residentes, media de los tiempos de ocho rondas con
orden alternado y tres calentamientos, tres instantes por chunk en las cuatro VGS SH2:

| Captura | Sin SH: anterior → nuevo | Menos tiempo | Con SH: anterior → nuevo | Menos tiempo |
|---|---:|---:|---:|---:|
| boxing | 2,595 → 2,283 ms | 12,0% | 7,456 → 6,586 ms | 11,7% |
| mario | 2,147 → 2,038 ms | 5,1% | 7,184 → 6,407 ms | 10,8% |
| boxingfull | 1,715 → 1,470 ms | 14,3% | 5,200 → 4,420 ms | 15,0% |
| tiki | 1,814 → 1,615 ms | 11,0% | 5,244 → 4,634 ms | 11,6% |

La prueba de DLL mide `schedule + acquire` sobre frames del primer chunk ya caliente,
con cuatro hilos del decoder, dos slots y tres rondas ABBA/BAAB. Incluye evaluación,
conversión y sincronización con el hilo nativo; excluye escribir atributos del host,
sorting y render. Cada combinación usa 144 observaciones por DLL. Medias de las cuatro
capturas, con igual número de observaciones por captura:

| Archivo / detalle | DLL anterior | DLL nueva | Menos tiempo |
|---|---:|---:|---:|
| VGS sin SH | 9,304 ms | 9,261 ms | 0,46% |
| VGS con SH | 24,308 ms | 23,995 ms | 1,29% |
| PGS sin SH | 9,463 ms | 9,386 ms | 0,82% |
| PGS con SH | 24,859 ms | 24,324 ms | 2,15% |

La mejora completa con SH va de 0,62% a 3,00% según captura/contenedor. Sin SH hay
variación, incluidas regresiones pequeñas: boxingfull VGS +1,69% de tiempo, mario
VGS +0,83% y tiki PGS +3,71%. No se promete mejora en cada ejecución ni un aumento
de FPS. Las diferencias entre VGS y PGS en esta prueba de frames calientes no miden
descompresión de chunks; no usar esa tabla para comparar la velocidad de sus codecs.

## Validación

- 1.728 casos sintéticos de conversión, comparados byte por byte con el recorrido
  anterior: cero registros, todos muertos, máscaras parciales, sin máscara, SH de
  grados 0/1/2/3, NaN, quaternion nulo, densidades y buffers que crecen y disminuyen.
- 996 comparaciones de frames entre DLLs, en las cuatro capturas VGS y PGS, con
  saltos/revisitas, cambios de SH y densidad; comprobación de memoria publicada
  mientras otra petición y un cambio de configuración se procesan en segundo plano.
- Identidad adicional al cronometrar la conversión de todos los chunks VGS.
- 22/22 pruebas CTest, incluidas las comprobaciones de ausencia de clave de firma
  en los binarios de Blender y Houdini.
- Blender 5.3.0 Alpha: 16 escrituras reales a PointCloud con lectura de vuelta de
  todos los atributos, igualdad de bits, cambios de SH/densidad y salto de chunk.
- Houdini 22.0.429: 12 cooks reales en hython con preferencias aisladas, cambios
  de SH, salto de chunk y revisita, comprobando puntos, atributos y ausencia de
  errores. El JSON registra el paquete y las DSO cargadas.

## Artefactos y reproducción

- `build_plugin_decode/plugins/blender/Release/vgs-1.0.0.zip`.
- `build_plugin_decode/plugins/houdini/Release/vgs-houdini-1.0.0-h22.0.zip`.
- `build_plugin_decode/reference/vgsblender_before.dll`: referencia previa.
- `build_plugin_decode/results/`: JSON de DLLs/hosts, CSV del packing y resumen.
- `tests/framepack.cpp`, `framepack_reference.h`: oráculo anterior, paridad y benchmark.
- `tests/player_compare.py`: usa el puente ctypes real del add-on para comparar DLLs.
- `tests/blender_player_smoke.py`, `houdini_player_smoke.py`: pruebas dentro de los hosts.

La referencia se guardó antes de modificar el player. Los paquetes se han generado
en el build; no se han instalado en el perfil del usuario.
No se ha medido reproducción interactiva en GUI ni rendering de escenas completas.

Comandos principales (desde la raíz; rutas del equipo de pruebas):

```powershell
cmake -S . -B build_plugin_decode -G "Visual Studio 17 2022" -A x64 `
  -DVGS_BUILD_PLUGINS=ON `
  "-DVGS_HOUDINI_ROOT=E:/SideFX/Houdini/Houdini 22.0.429" `
  "-DVGS_TEST_MINT=D:/Trabajos/ScanMeNow/SMNWebviewer/Playcanvas/public/data/boxing.mint"
cmake --build build_plugin_decode --config Release --parallel 4
ctest --test-dir build_plugin_decode -C Release --output-on-failure
build_plugin_decode/tests/Release/vgs_framepack.exe build_entropy_audit/boxing_standard.vgs
python tests/player_compare.py build_plugin_decode/reference/vgsblender_before.dll `
  build_plugin_decode/plugins/vgsblender/Release/vgsblender.dll `
  build_entropy_audit/boxing_standard.vgs build_plugin_decode/results/boxing_vgs.json
```

Para hython se usó `HOUDINI_PACKAGE_DIR` apuntando al paquete del build y
`HOUDINI_USER_PREF_DIR` a `build_plugin_decode/houdini_prefs__HVER__`. Houdini exige
ese token de versión para aceptar la ruta alternativa. La prueba aislada válida
está en `houdini_smoke_isolated.json`.
