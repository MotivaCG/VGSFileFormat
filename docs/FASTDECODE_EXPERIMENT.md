# Pruebas de decoding sin pérdida y sin cambios de trainer

Revisión cerrada el 3 de octubre de 2026. Entrada: boxing, mario, boxingfull y tiki,
sus MINT originales y exports Standard SH2. Equipo: Ryzen 9 7900X, Windows,
MSVC Release y Chrome 154.0.8037.92 con Emscripten O3/LTO y excepciones WASM.
Sin SIMD añadido al módulo de pruebas. Benchmarks ejecutados secuencialmente.

## Resultado principal

Decisión posterior a las pruebas: mantener los dos extremos existentes, VGS
comprimido y PGS (`--plain`). El usuario descartó añadir un perfil intermedio.
La siguiente línea de experimentos es reducir copias y buffers durante el packing
del viewer; véase [el experimento de packing](PACKING_COPY_EXPERIMENT.md).

Como resultado histórico, escoger Raw para atributos que rANS comprime poco produjo
archivos VGS normales con **21,8–32,7% menos tiempo** de preparación, a cambio de
**4,6–7,5% más tamaño**. Este selector queda descartado para integración; las tablas
se conservan como evidencia del experimento. No cambia trainer ni valores numéricos.

También se midió `.pgs`/Raw, que ya soporta el encoder normal: preparación **2,84–3,16
veces más rápida**, con **21,8–26,8% más tamaño** y mayor heap WASM.

LZ4 y las mezclas por página son prometedores, pero sus tiempos se han medido en
decoding aislado de atributos, no en un nuevo formato integrado en el viewer. No
confundir esas aceleraciones con las de un chunk entero o con FPS.

No se cambió el codec de producción ni su configuración por defecto. El nuevo
selector Raw/rANS está en un ejecutable experimental separado; LZ4 sólo se utiliza
en fixtures de benchmark, no se ha añadido a VGS.

## Preparación completa en navegador: perfil intermedio compatible

Regla fija del experimento: para cada atributo de todo el archivo, usar Raw si
sus bytes totales no superan en más de un 20% los del mejor candidato rANS.
Es un margen **por atributo**, no un aumento del 20% de todo el archivo. Mantiene
rANS donde comprime suficientemente bien. No necesita decisiones cronometradas
durante el export ni reglas particulares para boxing/mario/tiki.

| Captura | Standard | Intermedio | Menos tiempo | Más tamaño | Heap Standard → intermedio |
|---|---:|---:|---:|---:|---:|
| boxing | 290,88 ms | 227,57 ms | 21,8% | 5,18% | 126,9 → 133,4 MiB |
| mario | 179,95 ms | 130,03 ms | 27,7% | 4,68% | 93,9 → 98,4 MiB |
| boxingfull | 216,27 ms | 148,40 ms | 31,4% | 7,49% | 119,2 → 132,6 MiB |
| tiki | 171,65 ms | 115,57 ms | 32,7% | 4,64% | 103,1 → 115,5 MiB |

Tamaños reales firmados, no estimaciones: 40.389.600, 29.748.404, 208.612.632 y
327.456.192 bytes, respectivamente. Los exports conservan atributos y timeline;
el ejecutable experimental usa metadatos mínimos, no clona todos los metadatos del
export CLI de referencia. Las diferencias de metadatos son irrelevantes para los
porcentajes mostrados, pero estos archivos son pruebas y no entregas editoriales.

El heap de la tabla es la memoria reservada por el módulo WASM observada tras
preparar chunks. No es RSS del navegador ni VRAM, y no mide sólo memoria viva.
Los tamaños se repitieron exactamente al invertir el orden de comparación.

## Preparación completa: Raw / PGS ya existente

| Captura | Standard | PGS | Aceleración | Más tamaño | Heap Standard → PGS |
|---|---:|---:|---:|---:|---:|
| boxing | 276,93 ms | 87,76 ms | 3,16x | 26,79% | 126,9 → 160,8 MiB |
| mario | 186,55 ms | 65,77 ms | 2,84x | 23,59% | 93,9 → 116,1 MiB |
| boxingfull | 198,95 ms | 64,58 ms | 3,08x | 24,82% | 119,2 → 159,6 MiB |
| tiki | 172,05 ms | 59,64 ms | 2,88x | 21,80% | 103,1 → 126,2 MiB |

Tamaños reales PGS: 48.686.992, 35.120.292, 242.252.392 y 381.164.000 bytes.
PGS conserva los diccionarios, índices y valores cuantizados del contenido. Raw
significa que no se aplica el rANS de las páginas; no es una expansión a PLYs.

Las referencias Standard de ambas tablas se midieron en ejecuciones distintas.
No mezclar sus números para comparar directamente Intermedio contra PGS. Comparar
cada alternativa contra su Standard alternado, como se muestra en las tablas.

## Método y alcance de las mediciones completas

Mismo WASM de producción actualizado para los dos archivos de cada comparación.
Entrada residente en memoria; modo Packed y detalle Full. Una pasada de calentamiento
y tres rondas ABBA sobre todos los chunks. Segunda ejecución con los archivos
intercambiados. Cada tabla promedia ambas ejecuciones: 12/12/84/144 preparaciones
por alternativa para boxing/mario/boxingfull/tiki, respectivamente.

El tiempo incluye staging/copia hacia el decoder, CRC, directorios y preparación
de atributos. Excluye descarga, packing de texturas del viewer, subida a GPU,
sorting y render. La evaluación de cada instante no se acelera por cambiar sólo
el codec de página: los atributos reconstruidos y splats son los mismos.

Una exportación mayor puede cargar más lentamente por red aunque ahorre CPU.
Como ilustración, en un modelo estrictamente secuencial de descarga + prepare, los
bytes extra del perfil intermedio compensan aproximadamente a partir de 173–251
Mbit/s efectivos en esta máquina; para PGS, 405–444 Mbit/s. No es un umbral universal:
prefetch, caché, CPU del dispositivo, red y solapamiento cambian esa relación.

Por ello el perfil intermedio es candidato a un modo de exportación orientado a
decoding; PGS es especialmente relevante para archivos locales o ya descargados.
No se deduce que cualquiera de los dos deba reemplazar el export Standard por defecto.

## LZ4 y reorganización reversible: decoding aislado de páginas

2.683 páginas, 21 chunks. LZ4 upstream 1.10.0; HC nivel 9. Cada página se probó con:

1. Codec/modelo actual y payload original.
2. Raw.
3. LZ4 sobre bytes originales.
4. LZ4 HC sobre bytes originales.
5. Reorganización por bytes de palabras de 1/2/4 bytes seguida de HC.
6. Delta modular de esas palabras, reorganización por bytes y HC.

No hay cuantización ni aritmética float. Cada variante se compara byte a byte con
la página de referencia tras el calentamiento y tras cada ronda medida. Ocho rondas
rotan e invierten el orden de los codecs. WASM recibe las mismas alternativas
codificadas en nativo, hace un calentamiento adicional y verifica sus salidas.

| Variante | Tamaño estimado frente a VGS | Aceleración del decode de páginas WASM |
|---|---:|---:|
| LZ4 | +18,9 a +22,6% | 78,8–93,1x |
| LZ4 HC | +18,3 a +21,9% | 53,6–63,4x |
| Reorganización por bytes + HC | +15,8 a +19,2% | 15,7–16,7x |
| Delta + reorganización + HC | +16,4 a +19,5% | 8,2–9,1x |

Estas cifras grandes son sólo descompresión/reconstrucción de páginas calientes.
Incluyen asignar la salida y deshacer filtros, pero no CRC, staging, directorios,
packing ni GPU. No equivalen a un viewer 80 veces más rápido. LZ4 trata muchos de
estos datos casi como copias literales; rANS también reconstruye columnas, campos
y modelos numéricos. El coste del resto de la preparación limita la ganancia final.

HC reduce algo los bytes, pero también puede generar más referencias que cuestan
más al decoder: más esfuerzo de encoding no garantiza mejor tiempo de decoding.
Los filtros ahorran tamaño frente a LZ4 directo, pero su inversión consume CPU y
un buffer adicional del tamaño de la página. No asumir rendimiento SIMD no medido.

Los tamaños son estimaciones: payloads reales más el contenedor original y 16 bytes
adicionales por página como margen ilustrativo para describir una extensión. No se
ha diseñado/serializado todavía un contenedor LZ4 firmado.

## Mezcla de codecs por página, aún sin integrar

Se eligió la variante más rápida con **tiempos nativos**, limitando su crecimiento
local respecto a la página actual y contando 16 bytes extra de descriptor. Esas
decisiones se evaluaron después con **tiempos WASM**, sin volver a escoger el mínimo.

Con un máximo de 10% extra por página:

| Captura | Tamaño estimado del archivo | Aceleración de páginas en WASM |
|---|---:|---:|
| boxing | +1,18% | 1,21x |
| mario | +0,75% | 1,19x |
| boxingfull | +0,81% | 1,16x |
| tiki | +2,68% | 1,54x |

Con margen local de 20%, tamaño +6,6–8,0% y aceleración de páginas 1,81–2,55x.
Sin aumentar tamaño, la mejora es sólo aproximadamente 1–3%. Son sumas de costes
de páginas, no medidas de un decoder con dispatch y contenedor nuevos. Una política
por página o LZ4 exige extensión y lectores actualizados; el perfil Raw/rANS global
medido arriba no necesita esa migración.

## Validación y artefactos

Los exports PGS e intermedios se verificaron contra cada MINT. La prueba adicional
de navegador usa el decoder WASM anterior sobre Standard y el actualizado sobre
PGS/intermedio. Compara atributos ensamblados, texturas/metadatos de `packChunk()`
del viewer real, saltos y frames a tiempos 0/0,5/0,999 de cada chunk y revisitas.
No es un benchmark de rasterización ni un test de todos los plugins/dispositivos.
Pasaron las ocho comparaciones (cuatro PGS y cuatro intermedias), con **174
evaluaciones de frames** en total. Las seis variantes de páginas pasaron la
identidad exacta en nativo y WASM. También se verificó que el módulo WASM del
experimento no contiene la clave privada de firma.
Comprobaciones directas adicionales con lectores anteriores: WASM antiguo leyó
boxingfull intermedio y pasó otras 27 comparaciones de frames/packing; el ejecutable
nativo antiguo reprodujo boxing intermedio por sus diez rutas de evaluación,
Packed, sorting y paralelismo. No hubo cambios de decoder para habilitar el perfil.

- Código y reproducción: `experiments/fastdecode/README.md`.
- `build_fastdecode_results/summary.json`: codecs y políticas simuladas por página/atributo.
- `build_fastdecode_results/plain_summary.json`: preparación real de PGS.
- `build_fastdecode_results/balanced_summary.json`: preparación real del perfil intermedio.
- Por captura: `native.csv`, `native_replay.csv`, `wasm.csv`, JSON de cada ejecución
  y paridad, `plain.pgs`, `balanced.vgs` y bundles con los seis payloads.
- `build_fastdecode_native/Release/profileencode.exe`: encoder experimental firmado;
  regla de selección aplicada a una copia del source en el build, no al source normal.

Los bundles incluyen todas las alternativas y referencias y pueden ocupar varios
GB. El heap del banco de pruebas de LZ4 incluye esas copias; no se compara con el
heap del decoder real registrado en las tablas de preparación completa.
