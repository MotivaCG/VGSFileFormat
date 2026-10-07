# Revisión de decoder y alternativas sin cambiar el trainer

2 de octubre de 2026. Revisión del código actual, las mediciones conservadas y los
26 artículos locales. Restricción: trabajar sobre la salida existente del trainer;
no introducir entrenamiento de escenas, fine-tuning fotométrico ni nuevas redes
como requisito. Ajustar curvas o construir diccionarios en el encoder sí puede ser
una transformación posterior, pero sus errores y costes deben medirse.

Actualización del 3 de octubre: las [pruebas de codecs rápidos](FASTDECODE_EXPERIMENT.md)
ya midieron LZ4, filtros reversibles, PGS y un selector Raw/rANS por atributo. El
selector intermedio consiguió 21,8–32,7% menos tiempo de preparación completa con
4,6–7,5% más tamaño, usando el formato existente. El usuario descartó integrarlo:
se mantienen VGS y PGS como los dos extremos. Los siguientes experimentos se centran
en [reducir copias durante el packing](PACKING_COPY_EXPERIMENT.md), sin tocar el trainer.

## Qué esperar de lo implementado

| Cambio | Calidad | Tiempo | Compatibilidad y estado |
|---|---|---|---|
| Recorridos temporales del decoder | Valores exactos respecto al decoder anterior | Dos comparaciones WASM equivalentes dieron 3,35% y 2,25% menos tiempo medio de preparación completa | Misma sintaxis; beneficia archivos existentes al actualizar el decoder. Implementado |
| Páginas SH temporales divididas | Valores exactos respecto al export equivalente | Trabajo individual más corto; el tiempo total no mejora consistentemente | Lectores anteriores pueden leerlas. Opt-in y todavía no recomendado por defecto |
| Búsqueda de parámetros rANS | Sin pérdida adicional | Encoding 2,61–3,42 veces más lento en las cuatro exportaciones previas; ahorro de bytes 0,047–0,087% | Opt-in, sin ventaja demostrada para decoding |
| Tracks `adaptive/predictive` de `experiments/temporal` | Aproximación con pérdida; `samples` conserva los floats de entrada | Prototipo, no mejora demostrada frente al encoder MINT establecido | Encoding experimental 1: los lectores normales lo rechazan deliberadamente |

La identidad numérica se refiere al mismo MINT, al mismo grado SH y a las mismas
opciones de exportación. No recupera la información que el trainer/MINT ya hubiera
cuantizado. Los cambios compatibles no mejoran detalle visual, no reducen splats y
no prometen mayor FPS una vez el chunk está preparado. No hay cambios de trainer.

La mejora del 2–3% mide preparación completa de chunks residentes, con dos builds
equivalentes salvo los bucles temporales. Incluye staging, CRC y directorios;
excluye red, sorting, packing de texturas del viewer y render. La comparación
inicial contra un WASM archivado dio peor resultado: no debe presentarse esta
cifra como una aceleración ya demostrada de toda la aplicación distribuida.

## Nueva comparación de paginación en cuatro capturas

Se exportaron boxing, mario y tiki con `--sh 2 --split-temporal-sh` y se reutilizó
la exportación split de boxingfull. Todos conservan el mismo contenido que sus
exportaciones Standard. Los exports verifican sus atributos contra la fuente.

Chrome, mismo WASM actualizado en ambos lados, entrada residente y `Output::Packed`
con `Detail::Full`. Se calentaron todos los chunks y se hicieron tres rondas ABBA.
Una segunda ejecución intercambió el orden de los archivos. No hubo otros benchmarks
ejecutándose en paralelo. Tiempos de dos ejecuciones promediados:

| Captura | Preparación Standard | Preparación split | Cambio de tiempo | Cambio de tamaño |
|---|---:|---:|---:|---:|
| boxing | 291,52 ms | 293,96 ms | +0,83% | +0,082% |
| mario | 182,64 ms | 185,72 ms | +1,68% | +0,118% |
| boxingfull | 228,95 ms | 234,01 ms | +2,21% | +0,116% |
| tiki | 188,13 ms | 184,38 ms | −1,99% | +0,127% |

Signo positivo significa más lento. Hay 12, 12, 84 y 144 preparaciones por modo,
respectivamente. Ponderando por chunks preparados, el cambio es −0,093%: a efectos
prácticos, sin ganancia de throughput demostrada. Hay variación entre ejecuciones;
no extrapolar estos porcentajes a móviles, otras capturas o escenarios de red.

La medición anterior de boxingfull sigue siendo válida para tareas individuales:
7 páginas SH pasan a 33, la mediana por página baja de 6,102 a 1,211 ms y el máximo
de 7,329 a 1,724 ms. Cada operación hace menos trabajo, pero hay más operaciones.

La memoria reservada del módulo WASM merece atención:

| Captura | Heap Standard, bytes | Heap split, bytes |
|---|---:|---:|
| boxing | 133.038.080 | 133.103.616 |
| mario | 98.500.608 | 98.566.144 |
| boxingfull | 125.042.688 | 126.812.160 |
| tiki | 108.068.864 | 162.332.672 |

El resultado se repite exactamente al invertir el orden. En tiki son aproximadamente
103,1 frente a 154,8 MiB. Se midió el tamaño del buffer de memoria WASM observado al
final de cada prepare, no RSS del navegador, memoria viva exacta ni VRAM. La causa
del crecimiento no está aislada; podría depender de las asignaciones y del crecimiento
del heap. No afirmar que sea una fuga, ni que los atributos decodificados aumenten.

**Decisión:** mantener split desactivado por defecto hasta entender esa memoria y
medir la duración real de llamadas con presupuesto en el worker. Conservar la
optimización de recorridos y la herramienta de perfiles. Entropy-search queda
fuera del uso habitual.

Los datos están en `build_entropy_audit/review_split_summary.json` y los ocho
`review_split_*_bench.json`, con tiempos individuales y orden de comparación.

## Compatibilidad y límites de las comprobaciones

La última suite nativa pasó 19/19 pruebas: roundtrip, comparación con la fuente,
Packed/Floats, corrupción y separación de claves. El decoder nativo anterior leyó
los cuatro archivos entropy-search y boxingfull split. Las pruebas WASM previas
compararon atributos completos y frames, incluyendo revisitas y saltos.

El código del viewer real, `Playcanvas/src/vgs/vgspack.mjs`, ensambla las páginas
usando `firstRow/totalRows` y contempla SH por planos; no presupone una única página
de diccionario temporal. El script de revisión adicional usa esa función real de
packing para comparar también sus texturas y metadatos, además de los atributos
y frames entre WASM anterior/Standard y WASM actualizado/split.
Esa comprobación pasó en las cuatro capturas: 21 chunks distintos y 87 evaluaciones
de frame, incluyendo revisitas y tiempos fraccionarios. Los resultados están en
`build_entropy_audit/review_viewer_*_parity.json`; el script de esa revisión está
en `build_entropy_audit/review_viewer_parity.cjs`.

No equivale a probar todas las versiones instaladas de Blender/Houdini, todos los
dispositivos ni un render con métricas perceptuales. Los nuevos símbolos de la API
del encoder requieren una biblioteca actualizada para quienes los usen; un decoder
antiguo no necesita esos símbolos para leer los archivos compatibles.

## Trabajo adicional útil sin cambiar el formato

1. **Medir y reducir copias en la preparación.** `Capture::prepare()` copia el chunk
   y valida el directorio antes de empezar el reloj de su presupuesto por páginas.
   `store()` puede construir el evaluador al terminar. Un presupuesto de 2 ms no es
   un límite duro de 2 ms para la llamada entera. Paginar SH no resuelve esas fases.
2. **Medir packing y upload reales.** El viewer usa Packed, pero `vgspack.mjs` reúne
   páginas en arrays completos y después construye texturas. Reducir buffers
   intermedios o escribir en los destinos finales podría ahorrar CPU y memoria con
   calidad exacta. Es una oportunidad identificada por código, no una ganancia medida.
3. **Aislar el crecimiento del heap con split.** Revisar tamaños/ciclos de vida de
   tablas y buffers antes de decidir la paginación por defecto. No reducir páginas
   indiscriminadamente: más tareas y resets también tienen coste.
4. **Microbenchmarks adicionales:** ocho estados rANS frente a cuatro, reutilización
   de buffers y rutas especializadas. El lector acepta hasta ocho estados, pero no
   sabemos si compensa. La construcción de tablas fue sólo aproximadamente 2,4%
   del decode de atributos; la tabla rANS fusionada ya se descartó por coste mayor.

## Papers que sí justifican investigar cambios de formato

Ninguno ha demostrado ser mejor que este VGS sobre sus mismas capturas. Los
porcentajes de los artículos no son pronósticos de nuestros resultados. Las
siguientes son aplicaciones propuestas de sus ideas, no una reproducción de sus
sistemas completos.

### 1. TSOG: representación temporal explícita y atributos alineados

[TSOG, 2607.28049v1](https://arxiv.org/abs/2607.28049v1), secciones 3 y 4.
Es el encaje más directo: el formato es independiente del método de generación y
almacena lifespan, valores base y parámetros temporales por atributo en imágenes
alineadas. Sus experimentos incluyen conversión de PLYs y de una representación
FreeTimeGS que ya contiene velocidad. No demuestra que ajustar cualquier trayectoria
MINT con polinomios conserve calidad ni que decodifique más rápido que nuestro rANS.

Para VGS propondría selección posterior por pista/diccionario entre constante,
lineal, polinomio corto y muestras de respaldo, con evaluación directa en GPU. Puede
disminuir datos que reconstruir y transferir sin ejecutar una red. Debe conservar
lifetimes/generaciones y elegir usando coste final: MINT ya comparte trayectorias,
por lo que expandirlas primero por splat puede empeorar mucho el tamaño.

Requiere nuevos descriptores y semántica de evaluación, decoder y shaders. La
aproximación de curvas y recuantización añaden pérdida; pistas constantes exactas
pueden conservarse sin pérdida. El prototipo adaptive/predictive existente cubre
parte de esta dirección. Polinomios, selección ampliada y un camino de producción
GPU son el trabajo adicional. Prioridad alta como experimento de representación.

### 2. SplatStream / 4DGCPro: subconjuntos renderizables y entrega progresiva

[SplatStream, 2607.25971v2](https://arxiv.org/abs/2607.25971v2), sección III.E,
ordena splats por opacidad y volumen; [4DGCPro](https://arxiv.org/abs/2509.17513v2)
motiva capas perceptuales. El beneficio potencial es arrancar, ordenar y dibujar
menos splats cuando el dispositivo o la red lo requieren, en lugar de bajar sólo SH.

Podemos calcular importancia y empaquetar subconjuntos sobre splats ya entrenados.
La base pierde detalle y puede producir huecos/flicker; no hereda la calidad de
capas supervisadas durante entrenamiento. SplatStream completo usa supervisión y
un predictor aprendido, que quedan fuera de nuestra propuesta. Sus B-layer son
predicciones desde el anchor anterior, no B-frames bidireccionales.

Necesita particiones de splats, correspondencias entre atributos/términos,
dependencias de diccionarios y scheduling/renderer para contenido parcial. El
nivel completo podría conservar todos los datos originales, pero incluso el orden
puede afectar empates de rendering. Prioridad alta si el objetivo es arranque,
memoria y coste de render; no promete acelerar el decode de calidad completa.

### 3. CAGS: diccionarios con refinamientos de calidad

[CAGS, 2605.09279v1](https://arxiv.org/abs/2605.09279v1), sección 4.1.
Su Scalable Vector Quantization construye una jerarquía de codebooks e índices:
una base basta para una aproximación y nuevos bits refinan sus valores. Esa fase
offline se puede estudiar sobre los atributos existentes sin cambiar el trainer.

Sería útil para calidad progresiva de atributos y SH manteniendo todos los splats;
reduce el riesgo de huecos asociado a eliminar geometría, aunque introduce distorsión
de color/forma. No reduce por sí sola el número de splats a ordenar o rasterizar.
VGS ya usa codebooks: la novedad sería hacerlos escalables, no simplemente añadir VQ.

Requiere tablas e índices por nivel, nuevas dependencias y decoder actualizado.
El CAGS completo añade vídeo de referencia renderizado en servidor y una red de
restauración en cliente; sus mejoras grandes de PSNR no se transfieren a usar sólo
SVQ. No lo adoptaría completo para priorizar un decoder sencillo. Prioridad media.

### 4. CDGS: rangos locales y excepciones

[Constrained Dynamic Gaussian Splatting](https://arxiv.org/abs/2602.03538v1)
propone, entre otras cosas, compresión híbrida para manejar outliers. Podemos
adaptar rangos por bloque y escapes para que unos pocos extremos no obliguen a
gastar precisión en todos los valores. Es postproceso posible, con reconstrucción
simple; necesita metadatos de rangos, flags/escapes y nuevos modelos de página.

Es una opción con pérdida si recuantiza MINT; podría favorecer fuentes PLY de más
precisión. No eliminar extremos automáticamente. La estrategia central de CDGS,
que controla el presupuesto de splats durante entrenamiento, no es aplicable bajo
la restricción actual. Prioridad secundaria hasta medir potencial sobre nuestros datos.

## Qué no trasladaría como solución completa

- HGS, TED-4DGS, P-4DGS, 4D-MoDe, ReCon-GS, HPC, GS-DMSR, StreamLoD-GS,
  ClipGStream y DSD-GS dependen de representaciones o procesos aprendidos distintos.
  Separar constantes o predecir residuales sí puede inspirar postproceso, pero no
  autoriza a asumir sus ratios de compresión o velocidades.
- OMG4 parte de un modelo entrenado, pero optimiza entre pruning/merging y usa
  apariencia implícita: no es una conversión puramente posterior sin fine-tuning.
- PackUV optimiza en UV; StreamSTGS aprende features y deformación. Un atlas posterior
  es posible, pero no reproduce sus resultados. WebP/FFV1 lossless conserva las
  muestras del atlas, no elimina pérdida previa ni garantiza decode hardware.
- ProgressiveAvatars/HGC-Avatar requieren estructura y entrenamiento de avatar;
  PD-4DGS supervisa una descomposición progresiva. No son conversores genéricos MINT.
- DSGS genera splats en cliente mediante inferencia: va contra un decoder ligero.
  LentiAvatar, DLGStream y RAGA resuelven visualización lenticular, semántica y sombras.

Para extensiones incompatibles: conservar lectura del encoding actual en el nuevo
decoder y declarar explícitamente el nuevo encoding. Los lectores antiguos deben
rechazarlo, no interpretar silenciosamente los nuevos datos. La compatibilidad de
los cambios implementados no se extiende automáticamente a estas propuestas.

## Siguiente extensión para priorizar decoding sin cambiar el trainer

Antes de adoptar representaciones con pérdida, probaría una extensión de codecs
por página. El formato actual escoge Raw/rANS y modelo globalmente por atributo.
No puede mezclar libremente codecs/modelos entre páginas de ese atributo. El objetivo
de una nueva extensión sería seleccionar por latencia, memoria y bytes, con perfiles
de dispositivo medidos, en lugar de minimizar únicamente tamaño.

### Primera opción: Raw / rANS / LZ4 y transformación reversible

[LZ4](https://lz4.org/) está diseñado para decoding rápido y sin pérdida. Su variante
HC permite gastar más tiempo de encoding manteniendo el mismo formato de bloques
y decoder. Es una propiedad útil para contenido que se codifica una vez y se
reproduce muchas veces. Sus benchmarks genéricos no predicen el rendimiento VGS/WASM.

Compararía por atributo/página los bytes ya decodificados con Raw, rANS actual,
LZ4 y LZ4 precedido de una reorganización reversible por bytes/bits. El proyecto
[Bitshuffle](https://github.com/kiyo-masui/bitshuffle) es una referencia para esa
última transformación. El coste de deshacer la reorganización debe entrar en el
tiempo medido; las ventajas de SIMD nativo no se presuponen en todos los targets WASM.

Una representación de página directa más LZ4 podría evitar parte de la reconstrucción
de columnas/predictores del rANS actual, pero quizá comprima bastante peor. Se debe
conservar la alternativa actual y descartar páginas que no mejoren el objetivo.
Nada de esto está implementado ni tiene porcentajes de mejora medidos en este repo.

La extensión necesita codec/modelo/filtro por página, tamaños de salida acotados,
autenticación de esos descriptores y rechazo claro en lectores sin soporte. El
decoder nuevo debe conservar la lectura de los archivos actuales. Es un cambio de
sintaxis y dispatch, no requiere reemplazar todo el contenedor ni el trainer.

### Segunda opción: páginas listas para el camino GPU

Definir páginas cuyos bloques decodificados se puedan usar directamente como
buffers/texturas del renderer, o escribir en su destino definitivo, puede eliminar
el ensamblado intermedio que hoy hace `vgspack.mjs`. Parte de esta mejora podría
hacerse manteniendo el archivo actual; sólo cambiaría la representación cuando
mediciones demuestren que ese cambio de layout es necesario.

Sin recuantizar puede conservar los valores exactos. El coste puede ser padding,
duplicación de diccionarios compartidos o mayor tamaño de archivo. Un layout fijo
de texturas también puede atar el formato al renderer, por lo que conviene describir
bloques lógicos reutilizables y evitar imponer la anchura de texturas del viewer
actual. Hay que medir decode, packing, upload y memoria juntos.

Orden recomendado: medir estas opciones sin pérdida; después evaluar pistas de
TSOG y carga progresiva si se acepta pérdida en niveles reducidos. Una extensión
de formato sólo se promueve con mejora frente al VGS actual sobre las mismas
capturas, manteniendo acceso aleatorio y validación native/WASM.
