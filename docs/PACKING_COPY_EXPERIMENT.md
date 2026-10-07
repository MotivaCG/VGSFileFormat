# Experimento de copias al preparar texturas

3 de octubre de 2026. Se mantienen los dos extremos existentes: VGS comprimido y
PGS (`--plain`). Se experimenta con el packing de `vgspack.mjs` del viewer PlayCanvas,
después de preparar los atributos en WASM. No se cambia trainer, encoder, formato
de archivo, descompresor, shader ni calidad. El candidato está aislado del viewer
en uso; los resultados no significan que la aplicación distribuida ya lo incluya.

**Seguimiento completado:** el [nuevo ensayo dentro de un worker](PACKING_WORKER_FOLLOWUP.md)
midió dos sesiones por captura y completó la paridad de `directparts`. Favorece al
combinado: 4,99% menos tiempo total en VGS y 7,66% en PGS, de media en estas capturas.
Es el candidato preferido para integrar; sigue aislado del viewer en uso. Los datos
de abajo corresponden a la primera etapa y se conservan, incluidas sus regresiones.

**Decisión de la primera etapa:** conservar los prototipos y no activar todavía el candidato. Está
demostrado el ahorro de copias y asignaciones, y las comprobaciones de identidad
pasan. La mejora del tiempo completo es modesta y no estable: la prueba asíncrona
de boxingfull dio una regresión y su repetición dio mejora. No hay base para prometer
una aceleración general ni para cambiar el comportamiento por defecto del viewer.

## Qué se ha probado

1. **Reutilizar páginas completas durante el packing.** Si un atributo ya cabe en
   una sola página, leer esa vista en vez de crear otra copia. Se corrigen los accesos
   a SH y LUTs para respetar `byteOffset`. Las texturas de salida siguen teniendo
   sus propios buffers; ninguna vista prestada sale del empaquetador.
2. **Escribir directamente en las texturas finales.** Los buffers splat A/B/C y los
   índices SH se rellenan en su destino definitivo, incluyendo su padding a cero.
   Se elimina el buffer intermedio y su copia posterior.
3. **Concatenar grupos directamente en la textura.** Los términos de posición y
   rotación y los grupos con muestras explícitas se copian al destino sin `join()`.

Se comparan las tres variantes por separado y combinadas con una copia del packer
actual. No se elimina la copia para el sorter: sus datos tienen otro propietario.
Tampoco se elimina a ciegas la copia interna del chunk pendiente en el decoder:
mantiene su validez aunque se vuelva a alimentar la fuente durante un prepare parcial.
La fuente WASM ya proporciona `map()`, de modo que mover el scratch del lector nativo
no eliminaría esa copia del navegador.

## Medición

Ryzen 9 7900X, Chrome en Windows, mismo WASM actual y cuatro capturas SH2: boxing, mario,
boxingfull y tiki. Son 21 chunks, probados como VGS y como PGS. La entrada está
residente; la lectura HTTP de los archivos queda fuera de los cronómetros.

Packing aislado: tres llamadas de calentamiento por variante y doce rondas con
orden rotado e invertido por chunk. Se informa la media de las medianas por chunk.
Prepare + packing: calentamiento y tres rondas ABBA/BAAB, seis observaciones por
variante y chunk. La caché se vacía antes de cada preparación. Una ejecución separada
usa presupuestos de 8 ms y pausas MessageChannel, como el worker actual, aunque el
banco corre en una página sin las demás tareas de la aplicación.

El tiempo completo de esta prueba incluye staging, CRC, directorios, descompresión,
descripción del layout y packing. Excluye descarga, transferencia entre workers,
subida a GPU, sorting y render. No representa latencia de interfaz ni FPS.

Las copias se cuentan en módulos instrumentados fuera del cronómetro. Los bytes
de asignaciones suman los buffers explícitos solicitados durante una llamada; no
son memoria máxima simultánea, RSS del navegador, heap WASM ni VRAM. JIT, GC y carga
del equipo explican parte de la variación de tiempos: ambos contenedores producen
los mismos datos y usan el mismo algoritmo de packing.

## Buffers y copias eliminados

Media por chunk; es la misma para VGS y PGS porque sus atributos son idénticos.

| Captura | Asignaciones evitadas por chunk | Menos bytes copiados | Menos bytes asignados |
|---|---:|---:|---:|
| boxing | 45,9 MiB | 38,5% | 32,9% |
| mario | 29,5 MiB | 36,0% | 29,9% |
| boxingfull | 30,6 MiB | 37,0% | 31,6% |
| tiki | 26,3 MiB | 35,7% | 29,7% |

No hay cambios en el tamaño del archivo ni en las texturas finales. Se ahorra el
trabajo de crear y copiar intermediarios; no se ha medido el pico de RAM del viewer.

## Primera ejecución: variantes aisladas y preparación continua

Media de las medianas de packing por chunk, en milisegundos:

| Captura / archivo | Actual | Páginas prestadas | Destino final | Sin join | Combinado |
|---|---:|---:|---:|---:|---:|
| boxing VGS | 87,00 | 85,45 | 78,55 | 81,65 | 72,05 |
| boxing PGS | 85,50 | 88,90 | 80,70 | 79,30 | 76,70 |
| mario VGS | 75,35 | 71,00 | 78,25 | 68,90 | 62,60 |
| mario PGS | 49,25 | 48,40 | 47,20 | 47,10 | 44,65 |
| boxingfull VGS | 66,74 | 66,36 | 64,24 | 64,04 | 61,29 |
| boxingfull PGS | 37,56 | 67,30 | 61,39 | 66,61 | 29,22 |
| tiki VGS | 45,58 | 47,38 | 46,31 | 46,32 | 24,21 |
| tiki PGS | 29,75 | 50,44 | 45,62 | 48,94 | 24,30 |

Las variantes sueltas no aceleran consistentemente esta prueba. La combinación
mejora las ocho comparaciones, pero la magnitud varía mucho con el estado del
navegador. Por ejemplo, tiki VGS da una mejora aislada de casi 47%, que **no** se
traslada al tiempo completo. No usar ese máximo como expectativa de producción.

Preparación completa más packing continuo, medias por chunk:

| Captura | VGS actual → combinado | Menos tiempo VGS | PGS actual → combinado | Menos tiempo PGS |
|---|---:|---:|---:|---:|
| boxing | 419,47 → 423,50 ms | −0,96% | 213,38 → 192,17 ms | 9,94% |
| mario | 320,13 → 306,62 ms | 4,22% | 116,32 → 117,12 ms | −0,69% |
| boxingfull | 275,09 → 256,47 ms | 6,77% | 114,94 → 110,33 ms | 4,01% |
| tiki | 205,48 → 199,49 ms | 2,91% | 109,92 → 105,95 ms | 3,61% |

Los signos negativos significan más tiempo. Se conservan las pequeñas regresiones
observadas; no se seleccionan únicamente las capturas o métricas favorables.

## Preparación con pausas de 8 ms

Ejecución nueva, con ambas variantes calentadas y las pausas macrotask descritas:

| Captura | VGS actual → combinado | Menos tiempo VGS | PGS actual → combinado | Menos tiempo PGS |
|---|---:|---:|---:|---:|
| boxing | 438,88 → 415,68 ms | 5,29% | 194,27 → 187,37 ms | 3,55% |
| mario | 312,78 → 306,30 ms | 2,07% | 146,43 → 139,07 ms | 5,03% |
| boxingfull | 287,75 → 307,99 ms | −7,03% | 109,82 → 116,09 ms | −5,71% |
| tiki | 248,39 → 234,01 ms | 5,79% | 112,27 → 110,93 ms | 1,19% |

Se repitió boxingfull por esa regresión. VGS pasó de 278,57 a 273,38 ms y PGS de
123,60 a 110,64 ms. El packing de VGS pasó de 39,22 a 33,51 ms en la repetición,
frente a 42,12 → 60,32 ms en la primera ejecución. No hay cambios de código entre
ambas. La repetición favorable no invalida ni sustituye la regresión original.
No se ha aislado su causa; no atribuirla con certeza al GC, JIT o al equipo.

Como comprobación adicional se midió `directparts`: sólo destino final y eliminación
de joins, manteniendo las copias de entrada. En boxingfull asíncrono, VGS pasó de
282,95 a 280,17 ms y PGS de 121,92 a 113,64 ms. Es una medición exploratoria de un
tercer candidato, no un nuevo perfil de archivo. No se mezclan sus tiempos con
los del candidato combinado. En esta primera etapa no se había repetido en las cuatro
capturas ni aplicado la batería completa de paridad. Ambas se completaron después
en el seguimiento enlazado arriba; esta cifra exploratoria por sí sola no lo justificaba.

La conclusión práctica es que las copias son evitables, pero su ahorro no equivale
a una ganancia estable del descompresor. Los dos formatos existentes y sus opciones
permanecen intactos. El parche queda disponible para evaluación e integración futura;
no se han tocado los archivos del viewer que usa el usuario.

## Compatibilidad y límites

Pasaron 210 comparaciones de packing, 210 transferencias, 84 comparaciones de
packing asíncrono y 40 casos adicionales de paginación/grado/detalle. El comparador
recorrió 21.239.452.448 bytes contando las distintas comprobaciones repetidas.

Se comparan bytes de todas las texturas, padding, metadatos y paquete del sorter.
Se comprueba que las transferencias sean únicas, independientes de la memoria WASM,
y conserven el contenido después de transferir y separar sus buffers del emisor.
También se compara la ruta asíncrona con pausas en cada paso del generador.

En el primer chunk de cada captura y contenedor se fuerzan páginas partidas con
offset de memoria distinto de cero; se prueban grados SH 0/1/2 y detalle Base.
Las salidas se verifican después de liberar/repreparar la caché del decoder.
Es paridad con el packer existente, no una validación independiente de los shaders
ni una prueba de todas las capturas y dispositivos posibles.

El layout de entrada debe mantenerse vivo y sin cambios hasta terminar el packing.
El worker actual respeta ese contrato: sus pausas sólo permiten lecturas de descarga,
sin tocar el decoder de render. Un callback que libere la caché o haga crecer la memoria
del mismo módulo WASM durante el packing requeriría otro tratamiento de las vistas.

## Artefactos

- Generador y banco reproducible: `experiments/copybench/README.md`.
- Snapshot, variantes y hashes: `build_copybench/variants/`.
- Candidato combinado: `build_copybench/variants/combined.mjs`.
- Parche revisable para el repo del viewer: [PACKING_COPY_CANDIDATE.patch](PACKING_COPY_CANDIDATE.patch).
  Se comprobó que aplica sobre `src/vgs/vgspack.mjs` sin modificarlo.
- Resultados detallados: `build_copybench/run1/`, `build_copybench/async1/`,
  `build_copybench/async2/` (repetición de boxingfull) y `build_copybench/directparts/`.
- Resumen de la primera ejecución y la prueba asíncrona: `build_copybench/summary.json`.

`experiments/` y `build*/` están ignorados por la configuración actual del repositorio.
Conservar explícitamente esos artefactos si se quieren trasladar a otra máquina.
