# Aplicación de los papers de compresión a VGS

Revisión técnica, 2 de octubre de 2026. Fuente: los 26 PDF de
`C:/Users/Victor/Downloads/queen-main/compressionpapers`, en las versiones indicadas
abajo. Se extrajo su texto completo y se revisaron los métodos y las limitaciones
relevantes para el codec. Esto no reproduce sus implementaciones ni valida sus resultados.
Las propuestas para VGS son inferencias de ingeniería, no mejoras medidas.

La [revisión posterior del decoder](DECODER_EXPECTATIONS_REVIEW.md) incorpora la
restricción de no cambiar el trainer, las mediciones de paginación sobre cuatro
capturas y el coste de memoria WASM detectado. Sus decisiones actualizan la
prioridad de las propuestas iniciales de este documento.

## Decisión

Sí hay material aprovechable. Priorizaría:

1. Medir y optimizar la codificación sin pérdida que ya existe: tablas rANS,
   reparto de páginas, selección de modelos y coste de preparación.
2. Ampliar el experimento temporal existente con elección de representación por
   atributo/pista, separando valores constantes, movimiento simple y residuales.
3. Diseñar calidad progresiva que reduzca también los splats y el movimiento,
   además de los armónicos esféricos. Requiere cambios de representación y viewer.

Los resultados de 40x, 90x o >90% de reducción de algunos artículos comparan otras
representaciones, entrenamiento y/o archivos PLY. No predicen el ahorro sobre un VGS
que ya usa cuantización, diccionarios, términos residuales y rANS.

## Qué tenemos realmente

La implementación actual tiene chunks independientes, páginas por atributo,
diccionarios de trayectorias y SH, deltas temporales, contextos por canales y
cuaterniones, términos de diccionario y cuatro estados rANS intercalados. Hay
selección Raw/rANS y de modelo, pero **global por atributo para todo el archivo**.
El encoder suma el coste real de todos los candidatos antes de seleccionar.

El decoder ya ofrece `Detail::Positions/Base/Full`, preparación por presupuesto,
paralelismo por páginas, caché limitada y `Output::Packed`. Este último conserva
páginas decodificadas para evaluación por el shader; no equivale a que toda la
cadena hasta GPU carezca de copias o repacking.

Fuentes concretas:

- `core/src/vgsencode.cpp`, `pages()` y `encodeSource()`: paginación y políticas.
- `core/src/mgscodec.cpp`, `encodeSymbols()`, `Tables::read()`, `encodeWide()`:
  tablas, estados rANS y separación de bits.
- `core/src/vgscontainer.cpp`: validación y dispatch de páginas.
- `core/src/vgsframe.cpp`: semántica de reconstrucción y ensamblado de atributos.
- `decoder/src/vgsdecoder.cpp`: Packed, preparación y paralelismo.
- `decoder/wasm/vgswasm.cpp`: exposición de los mismos buffers en WASM.
- `experiments/temporal/README.md`: pistas adaptativas y base/step/residual ya
  existentes, con evaluación GPU experimental y límites de validación explícitos.

`docs/NUMERICAL_CODING.md` conserva referencias históricas a MGS y otra estructura
de proyecto. Sirve para los modelos numéricos reutilizados; el contenedor vigente
se determina por `FORMAT.md` y el código. No hay que tratarlo como otro decoder VGS.

## Medición de referencia en una captura real

Ejecutado el binario existente, sin recompilar ni modificar codec:

```powershell
.\build_temporal_audit\decoder\Release\vgspagecost.exe `
  'D:\Trabajos\ScanMeNow\SMNWebviewer\Playcanvas\public\data\boxing_despill.vgs' `
  --rounds 2 --threads 1
```

Archivo: 194.074.392 bytes, 7 chunks, duración media de chunk 1,01 s,
máximo 247.331 splats, grado SH 2. La herramienta rotula MB, pero calcula MiB.

| Medida | Resultado de esta ejecución |
|---|---:|
| Payload base / SH estático / SH temporal | 139,9 / 18,5 / 26,6 MiB |
| Preparación media Positions / Base / Full | 76,5 / 203,9 / 255,9 ms |
| Peor preparación Positions / Base / Full | 91,8 / 265,5 / 339,6 ms |
| Evaluar posiciones / color base / con SH | 4,44 / 20,49 / 52,84 ms |
| `position_trajectories` almacenado | 22,9 MiB |
| `position_rq_coefficients` almacenado | 20,9 MiB |
| `sh_static_indices` + `sh_temporal_indices` | 36,4 MiB |
| `rotation_samples` almacenado | 15,9 MiB |
| `position_samples` almacenado | 0,2 MiB |

Interpretación: en esta captura, optimizar únicamente las muestras directas de
posición tendría poco impacto. Importan las trayectorias compartidas, sus términos,
los índices SH y las rotaciones. Saltar todos los SH evita aproximadamente el 24%
del payload, pero deja los mismos splats base. El coste de evaluar con SH también
justifica continuar la vía Packed/GPU.

Dos rondas, una captura y un binario previo dan una referencia orientativa. No son
un benchmark de una propuesta, no prueban el estado exacto del código modificado
en el workspace y no representan Android, XR ni una red real. El archivo se lee a
memoria antes de cronometrar; no se midieron rasterización ni sorting.

## Propuestas concretas y compatibilidad

### A. Optimizar rANS sin cambiar la reconstrucción

Los papers enfatizan modelar cada atributo y pagar el coste real del bitstream.
VGS ya hace parte de esto. La oportunidad concreta se encuentra en la implementación:

- `encodeSymbols()` fija `bits = clamp(bitlen(A-1)+3,12,16)`.
  Probar varias precisiones legales y seleccionar por bytes de tablas + streams.
  `Tables::read()` acepta 12..16, de modo que no exige una nueva sintaxis para
  los lectores examinados. Debe mantenerse `M >= símbolos con frecuencia no cero`
  y verificarse también cada decoder distribuido. El documento numérico prescribe
  hoy una fórmula fija: habría que actualizar esa parte si se promueve el cambio.
- `encodeWide()` sólo prueba tres separaciones alto/bajo. Probar otros cortes
  legales podría mejorar el tamaño sin cambiar la interpretación del payload.
- El decoder admite 1..8 estados; el encoder escribe 1 o 4. Comparar 4 frente a 8
  puede ser una variante compatible con el lector examinado, pero añade estados,
  longitudes y flushes. Elegir por velocidad y bytes medidos, no por intuición.
- Medir cuánto cuestan construir y liberar tablas frente al bucle rANS.
  Reutilizar capacidad de buffers dentro de cada worker es una optimización del
  decoder; compartir tablas entre páginas en el archivo sí requeriría otro diseño.

Estas son propuestas propias motivadas por la revisión, no algoritmos que los
papers hayan probado directamente en nuestro rANS. Mantener siempre el candidato
actual permite no empeorar bytes cuando se elige exclusivamente por tamaño; no
garantiza que la búsqueda adicional compense en tiempo de encoding.

### B. Páginas y modelos adaptativos

Hay una discrepancia concreta: el comentario previo a `pages()` habla de gather de
entradas SH temporales, pero la rama `family == 2` copia todo el atributo en una
sola página y hace `continue`. Un atributo grande puede ser una tarea indivisible;
`prepare()` sólo puede detenerse entre páginas/lotes.

Primero medir el peor tiempo por página y variar `pageRows`. Para dividir el SH
temporal hay que conservar sus strides y el layout sample-major. Un gather de un
subconjunto de entradas seguido de concatenación lineal **no reconstruye** la
disposición original. La extensión debe definir scatter/indices o segmentos
contiguos correctamente descritos y probar la reconstrucción y la GPU. No dar
por hecha su compatibilidad por existir `firstRow`.

Calcular además el ahorro teórico de escoger Raw/modelo por página frente al
modelo global actual. Las políticas vigentes no transportan ese override: aunque
sea una mejora sin pérdida, requiere negociación/extensión explícita y lectores
actualizados. No insertar un byte de modelo en el payload actual.

### C. Pistas temporales por atributo

Inspiración principal: [TSOG](https://arxiv.org/abs/2607.28049v1),
[HGS](https://arxiv.org/abs/2512.14352v1),
[TED-4DGS](https://arxiv.org/abs/2512.05446v1) y
[P-4DGS](https://arxiv.org/abs/2510.10030v1).

Probar, por pista: constante, base+velocidad, polinomio de orden bajo, knots
adaptativos y predictor+residual. Elegir por bytes finales bajo un límite de error
visual, incluyendo descriptores, tiempos, generaciones y alineación. Una pista de
color puede ser constante aunque su posición sea dinámica. Reutilizar la misma
identidad sólo durante la vida de la misma generación; nunca interpolar un slot
reciclado como si fuera el mismo splat.

Esto continúa `adaptive` y `predictive` existentes. Los modos actuales ya incluyen
knots y base/step con residuales int16: no presentar esas ideas como nuevas.
La nueva parte sería ampliar candidatos, separar constantes explícitamente y
seleccionar con coste real y calidad de render. La variante de polinomios necesita
una semántica de tracks nueva; no se añade automáticamente al encoding 0.

En cuaterniones, resolver q/-q, normalización y cambios de componente dominante.
Comparar error angular y frames intermedios; un ajuste excelente de los cuatro
componentes por separado no asegura buena rotación. No imponer opacidad temporal
gaussiana a todos los splats: las ventanas suaves de otros métodos son otra
semántica frente a nuestros lifetimes discretos.

### D. Cuantización local y manejo de outliers

[CDGS](https://arxiv.org/abs/2602.03538v1), sección de compresión híbrida,
explica cómo los outliers amplían el rango y perjudican cuantización. Transferir
la idea de rangos por bloque/atributo, con escapes para excepciones, a la vía PLY
experimental. Medir todos los bytes de ranges, flags y escapes.

No eliminar automáticamente el 5% extremo ni usar un umbral 3-sigma como regla
universal: puede contener pelo, manos o detalles importantes. Si se introduce
cuantización nueva, definir error espacial, angular, de escala y color, y validar
renders comunes. El import MINT ya llega cuantizado; recuantizarlo es otra pérdida.

### E. Progresivo que reduzca coste de render

[4DGCPro](https://arxiv.org/abs/2509.17513v2),
[SplatStream](https://arxiv.org/abs/2607.25971v2),
[ProgressiveAvatars](https://arxiv.org/abs/2603.16447v1) y
[PD-4DGS](https://arxiv.org/abs/2605.11427v1) motivan una base renderizable y
refinamientos. En VGS las capas actuales separan base/SH estático/SH temporal;
no proporcionan automáticamente LoD de geometría ni movimiento.

Diseñar una base de menos splats y capas adicionales con dependencias declaradas,
datos compartidos disponibles y metadatos autenticados. Una puntuación inicial
puede combinar opacidad y volumen; mejorarla con cobertura proyectada e impacto
en renders de varias cámaras. Evitar cambios de selección bruscos entre frames.

Reordenar registros exige arrastrar **todos** los atributos, términos variables y
límites de rango. No hacer un Morton sort de posiciones aisladas: en MINT los
splats están agrupados por rangos de términos y la permutación debe respetar o
reconstruir esa organización. El orden también afecta empates del renderer.

Reducir splats sin reentrenar es un experimento con pérdida; la calidad monótona
de los prefijos no está garantizada. Varios artículos supervisan cada nivel durante
entrenamiento. La arquitectura de capas de VGS ayuda a construir la extensión,
pero ni declarar una capa vendor ni marcar atributos opcionales crea por sí solo
un camino de render compatible.

En SplatStream v2 las llamadas B-layer se codifican con predicción tipo P desde
el anchor anterior. No son B-frames bidireccionales reales. Su entrada a la cadena
temporal necesita el anchor de calidad completa; un prefijo intra pequeño no
equivale automáticamente a una cadena temporal completa de bajo bitrate.

### F. Mantener chunks independientes al introducir predicción

ReCon-GS, ClipGStream, HPC y AirGS explotan coherencia temporal con distintos
grados de herencia. Para nuestro acceso aleatorio, empezar por predicción dentro
del chunk con reset explícito. Predicción entre chunks sólo con referencias,
keyframes, límites de cadena y coste de seek cuantificados. Ahorrar bytes a costa
de decodificar todo el pasado contradice el modo de acceso actual.

Separar estático/dinámico dentro de un chunk es más sencillo que persistir un
fondo entre chunks. Esta última opción necesita otro lifetime, caché y grafo de
dependencias; el import actual ya comparte trayectorias y diccionarios, de modo
que no debe asumirse un ahorro igual al de un baseline por-frame.

### G. Atlases 2D y codecs de imagen/vídeo como experimento posterior

StreamSTGS, PackUV, AirGS y TSOG muestran interés en organizar atributos como
imágenes. Conservar alineación por identidad y hacer decodificación directa a
texturas puede ser útil. Pero PackUV optimiza en UV y con baja precisión durante
training: no promete el mismo resultado proyectando después cualquier MINT.

Comparar un atlas con rANS sobre exactamente los mismos valores, e incluir padding,
metadatos, transporte, decoder, repacking y VRAM. WebP lossless o FFV1 preservan
las muestras que reciben, no deshacen cuantización previa. No asumir que HEVC,
H.264 o FFV1 están disponibles acelerados en todas las plataformas del decoder.
Los cortes temporales y dependencias del codec de vídeo también afectan seeks.

## Inventario de los 26 artículos

Los IDs enlazan la versión local revisada. La columna final distingue ideas
transferibles de sistemas que requieren otro entrenamiento/renderer.

| ID/version | Trabajo | Aplicación al proyecto |
|---|---|---|
| [2509.17506v1](https://arxiv.org/abs/2509.17506v1) | 4D-MoDe | Estático/dinámico, keyframes adaptativos y compensación de nuevo contenido; grids+MLP requieren otra representación. |
| [2509.17513v2](https://arxiv.org/abs/2509.17513v2) | 4DGCPro | Capas perceptuales, movimiento jerárquico y supervisión por nivel; útil para un futuro LoD de geometría. |
| [2509.24325v2](https://arxiv.org/abs/2509.24325v2) | ReCon-GS | Movimiento compartido coarse-to-fine y reconfiguración de anchors; candidato de predictor experimental. |
| [2510.03857v1](https://arxiv.org/abs/2510.03857v1) | OMG4 | Pruning, merging y **Sub-Vector Quantization**; reducción de representación con pérdida/optimización. |
| [2510.10030v1](https://arxiv.org/abs/2510.10030v1) | P-4DGS | Predicción espacial/temporal, cuantización por atributo y entropía contextual; no transplantar el MLP al decoder actual. |
| [2510.16463v1](https://arxiv.org/abs/2510.16463v1) | HGC-Avatar | Pose SMPL-X + estructura generada; formato de avatar articulado especializado, no compresor genérico de captures. |
| [2511.06046v1](https://arxiv.org/abs/2511.06046v1) | StreamSTGS | Grids/atlases y features temporales como vídeo; alineación temporal y ruta directa a GPU. |
| [2512.05446v1](https://arxiv.org/abs/2512.05446v1) | TED-4DGS | Activación temporal y banco de deformación compartido; conservar lifetimes y distinguir pista constante de dinámica. |
| [2512.14352v1](https://arxiv.org/abs/2512.14352v1) | HGS | Parámetros estáticos compartidos, RBF temporal y trayectorias explícitas; inspiración para modos por pista. |
| [2512.20943v1](https://arxiv.org/abs/2512.20943v1) | AirGS | Keyframes y pruning de updates según ancho de banda; requiere identidad, dependencias y cliente adaptativo. |
| [2601.05584v1](https://arxiv.org/abs/2601.05584v1) | GS-DMSR | Optimización sensible a dinámica y modelado multiescala; utilidad principal en entrenamiento, menor en nuestro bitstream. |
| [2601.18475v1](https://arxiv.org/abs/2601.18475v1) | StreamLoD-GS | Anchors/octree, congelar estáticos y residuales cuantizados; referencia para LoD y movimiento compartido. |
| [2602.00671v1](https://arxiv.org/abs/2602.00671v1) | HPC | Latentes jerárquicos y deltas de pesos de red; útil si adoptamos redes, poco directo para un decoder explícito sin ellas. |
| [2602.03538v1](https://arxiv.org/abs/2602.03538v1) | Constrained Dynamic Gaussian Splatting | Presupuesto de splats y separación de outliers; presupuesto útil para móviles y rangos locales experimentales. |
| [2602.23040v2](https://arxiv.org/abs/2602.23040v2) | PackUV | Identidad en UV, keyframing y baja precisión durante fitting; estudiar atlases sin prometer equivalencia post-hoc. |
| [2603.16447v1](https://arxiv.org/abs/2603.16447v1) | ProgressiveAvatars | Jerarquía por subdivisión y ranking perceptual; progresivo útil, asociación a FLAME específica de cabezas. |
| [2604.13746v1](https://arxiv.org/abs/2604.13746v1) | ClipGStream | Coherencia entre clips y anchors de compensación; atender flicker y continuidad entre chunks. |
| [2605.09279v1](https://arxiv.org/abs/2605.09279v1) | CAGS | **Scalable Vector Quantization**, refinamiento de codebooks; restauración con imágenes de referencia implica servidor/viewer nuevos. |
| [2605.11427v1](https://arxiv.org/abs/2605.11427v1) | PD-4DGS | Scaffold estático, movimiento global y detalle local; valorar calidad temporal de cada prefijo. |
| [2605.17002v1](https://arxiv.org/abs/2605.17002v1) | Decoder-Side Gaussian Splatting (DSGS) | Generar splats en cliente desde imágenes; reemplaza la función del decoder, baja prioridad para VGS. |
| [2605.30863v1](https://arxiv.org/abs/2605.30863v1) | DSD-GS | Descomposición estático/dinámico durante reconstrucción; útil upstream, no una receta de entropy coding. |
| [2606.10550v2](https://arxiv.org/abs/2606.10550v2) | LentiAvatar | Captura de cabeza y salida lenticular; sin mejora directa del codec VGS. |
| [2606.28840v1](https://arxiv.org/abs/2606.28840v1) | DLGStream | Interpolación y features de lenguaje opcionales; interesante extensión semántica, no prioridad de compresión base. |
| [2606.29329v1](https://arxiv.org/abs/2606.29329v1) | RAGA | Sombras en espacio gaussiano y proxy de avatar; renderer, no encoder/entropy decoder. |
| [2607.25971v2](https://arxiv.org/abs/2607.25971v2) | SplatStream | Paquetes por importancia y capas temporales; estudiar dependencias y la limitación B-layer señalada. |
| [2607.28049v1](https://arxiv.org/abs/2607.28049v1) | TSOG | Lifetime, parámetros temporales explícitos y atributos alineados; referencia más directa para un formato 4D genérico. |

No confundir los dos usos de SVQ: OMG4 usa Sub-Vector Quantization; CAGS usa
Scalable Vector Quantization. Son técnicas diferentes.

## Secuencia de experimentos propuesta

| Orden | Experimento | Condición para promover |
|---|---|---|
| 1 | Instrumentar y acelerar reconstrucción temporal y preparación completa de chunks | Identidad exacta, menor latencia native/WASM con builds equivalentes, memoria aceptable. |
| 2 | Dividir páginas SH temporales y medir el peor bloqueo por página | Menor latencia por unidad de trabajo; contabilizar directorios/tablas y verificar lectores existentes. |
| 3 | Extender tracks experimentales con constantes y polinomios | Ganancia frente a `samples/adaptive/predictive` y encoder establecido a igual calidad. |
| 4 | Rangos locales y residuales con escapes | Error visual aceptado y ganancia que pague flags/ranges/escapes. |
| 5 | Geometría/movimiento progresivos | Cada nivel renderizable, menos bytes y menos coste de render, sin flicker excesivo. |
| 6 | Atlases/codec de vídeo | Mejora end-to-end en navegador y dispositivos objetivo, incluyendo seeks y VRAM. |

La prioridad indicada después por el usuario es el decoding. La búsqueda de
precisión rANS y cortes de bits queda como experimento opt-in: reducir bytes por
sí solo no compensa aumentar la latencia o memoria del decoder.

Para cambios sin pérdida: comparar atributos exactos, todas las muestras y seeks;
usar los checks existentes de roundtrip, corrupción, firma y separación de claves,
y paridad native/WASM. Para variantes con pérdida: congelar entrada, cámaras y
color fit, comparar PSNR/SSIM/LPIPS, detalles finos y flicker, e incluir tiempos
fraccionarios y fronteras de generación/chunk. Medir tamaño final firmado,
latencia inicial, p95/peor preparación y memoria CPU/GPU con el mismo viewer.

El experimento temporal ya declara presupuestos de pérdida <=0,2 dB y <=0,5
unidades añadidas de flicker y limitaciones de la comparación GPU. Mantener esos
gates como punto de partida; no promover por una reducción de bytes aislada ni
comparar únicamente con su baseline `legacy`, que no representa el mejor MINT/VGS.

En la revisión inicial se añadió documentación y se ejecutó la medición de
referencia. La implementación posterior de búsqueda rANS, instrumentación y
paginación SH, con sus resultados y compatibilidad, está documentada en
[experiments/entropy/README.md](../experiments/entropy/README.md).
