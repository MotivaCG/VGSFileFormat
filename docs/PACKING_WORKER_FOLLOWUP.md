# Seguimiento: packing en un worker y variante sin vistas prestadas

3 de octubre de 2026. Continuación de [las primeras pruebas de copias](PACKING_COPY_EXPERIMENT.md).
La primera etapa ahorraba buffers, pero boxingfull alternaba entre mejora y regresión.
Faltaba validar por completo `directparts`, que conserva las copias de entrada.

**Resultado:** el candidato combinado reduce el tiempo medio de preparación más
packing un **4,99% en VGS** y un **7,66% en PGS** en este banco. Su packing baja un
21,01% y un 14,14%, respectivamente. `directparts` obtiene 1,66% y 6,74% de mejora
total, y no ofrece una estabilidad claramente superior. Ambas combinaciones tienen
paridad exacta con el original en las comprobaciones realizadas.

**Decisión:** el combinado es el candidato preferido para integrar. Este seguimiento
aporta evidencia favorable adicional; no garantiza una mejora en cada ejecución o
dispositivo. Sigue como prototipo y parche revisable: estos experimentos no han
modificado el viewer en uso ni sus valores por defecto.

## Resultados

Medias de las dos sesiones, ponderadas por los chunks medidos (21 por contenedor;
boxing y mario tienen uno, boxingfull siete y tiki doce). No es una media de todos
los dispositivos ni una comparación de FPS.

| Implementación | Preparación + packing VGS | Menos tiempo | Preparación + packing PGS | Menos tiempo |
|---|---:|---:|---:|---:|
| Actual | 289,22 ms | — | 142,08 ms | — |
| Combinado | 274,79 ms | 4,99% | 131,20 ms | 7,66% |
| Sin prestar páginas (`directparts`) | 284,43 ms | 1,66% | 132,50 ms | 6,74% |

Detalle del candidato combinado, medias por chunk:

| Captura | VGS actual → candidato | Menos tiempo VGS | PGS actual → candidato | Menos tiempo PGS |
|---|---:|---:|---:|---:|
| boxing | 408,59 → 402,78 ms | 1,42% | 189,38 → 169,03 ms | 10,75% |
| mario | 281,92 → 280,26 ms | 0,59% | 137,94 → 134,08 ms | 2,80% |
| boxingfull | 306,40 → 297,72 ms | 2,83% | 141,90 → 133,37 ms | 6,01% |
| tiki | 269,87 → 250,30 ms | 7,25% | 138,59 → 126,54 ms | 8,70% |

Para conservar visible la variación, reducción porcentual de tiempo en cada sesión
(primera / orden invertido; negativo significa regresión):

| Captura / contenedor | Combinado | `directparts` |
|---|---:|---:|
| boxing VGS | 1,78 / 1,00% | 7,48 / −1,88% |
| boxing PGS | 12,73 / 8,44% | 7,42 / 9,14% |
| mario VGS | −1,64 / 2,93% | 1,73 / 0,27% |
| mario PGS | 1,98 / 3,67% | −3,84 / 4,79% |
| boxingfull VGS | 5,31 / 0,16% | 2,61 / −0,73% |
| boxingfull PGS | 5,10 / 6,95% | 4,24 / 5,76% |
| tiki VGS | 2,50 / 12,10% | 2,48 / 1,43% |
| tiki PGS | 10,88 / 6,47% | 10,81 / 5,43% |

La regresión anterior de boxingfull no se reproduce en estas dos sesiones para el
combinado, aunque una de las mejoras VGS es casi nula. Sigue habiendo una regresión
leve en mario VGS y diferencias apreciables entre sesiones. No interpretar la media
como una ganancia mínima garantizada. Se evita entre 26 y 46 MiB de asignaciones
temporales por chunk en el combinado, como en la primera prueba; no se midió el pico
de RAM del navegador.

## Qué cambia en la prueba

El mismo decoder WASM prepara las capturas en un **worker de navegador real**. Ese
worker prepara y empaqueta en bloques de 8 ms y después transfiere las texturas a
la página receptora, que conserva el último resultado hasta recibir el siguiente.
La transferencia y su confirmación quedan fuera del cronómetro. No hay GPU,
sorting, render ni otras tareas del viewer compitiendo dentro de este banco.

Se comparan tres implementaciones:

- `baseline`: packer actual del viewer.
- `combined`: vistas temporales de páginas completas, escritura en texturas finales
  y concatenación sin buffers intermedios.
- `directparts`: los dos últimos cambios; conserva las copias de las páginas.

Diez packings de calentamiento por implementación sobre el primer chunk visitado,
seguidos de una pasada completa por todos los chunks e implementaciones. Se miden
seis pasadas, utilizando las seis permutaciones posibles de las tres implementaciones:
cada una ocupa cada posición dos veces por chunk. Se repite en una sesión nueva de
Chrome invirtiendo orden de contenedores, recorrido de chunks y permutaciones.

Se prueban boxing, mario, boxingfull y tiki como VGS y PGS. Son 21 chunks por
contenedor. Las dos sesiones aportan **1.512 preparaciones medidas** y **2.244
transferencias de salida**, contando el calentamiento. El source, WASM y packers
quedan identificados por SHA-256. Las ejecuciones son secuenciales en el Ryzen 9 7900X.

## Qué permite concluir sobre las pausas

Se mide por separado el tiempo transcurrido esperando cada callback MessageChannel.
El tiempo de packing menos esas esperas sigue incluyendo GC y desalojo del hilo
por el sistema operativo; **no es un contador de tiempo CPU**. Permite separar
esperas explícitas del resto, pero no identificar por sí solo una desoptimización
del JIT, una recogida de basura o actividad de otros procesos.

Con VGS, las esperas medias del packing pasan de 2,44 a 1,59 ms en el combinado,
y el resto del packing de 62,23 a 49,48 ms. Con PGS, las esperas pasan de 2,64 a
1,82 ms y el resto de 64,32 a 55,68 ms. Por tanto, en este experimento la reducción
no se explica sólo por esperar menos entre pausas: también baja el trabajo transcurrido
fuera de ellas. El presupuesto de 8 ms es cooperativo, no un límite estricto para
asignaciones, GC o preempciones.

El banco cambia varias condiciones respecto al anterior: worker, calentamiento,
orden de observaciones y vida de las salidas. Por eso una mejora en esta prueba no
demuestra que alguna de esas condiciones sea, por sí sola, la causa del resultado
anterior. No se descartan las regresiones previas ni se sustituyen sus mediciones.

## Calidad y compatibilidad

Se añade la batería completa de identidad para `directparts` en los 42 chunks
contenedor/captura: bytes y metadatos, transferencias sin compartir memoria WASM,
packing asíncrono, paquete del sorter, paginación sintética con offset distinto de
cero, grados SH 0/1/2, detalle Base y conservación de salidas al liberar la caché.

Pasaron **84 comparaciones de packing, 84 transferencias, 84 comparaciones asíncronas
y 40 casos adicionales** de páginas/grados/detalle. Los contadores incluyen baseline
y candidato. Se compararon 11.744.081.696 bytes sumando las comprobaciones repetidas;
el combinado ya había pasado su batería en la etapa anterior.

No se cambia el trainer, el encoder, la sintaxis del archivo, la precisión ni los
shaders. Las texturas finales siguen siendo propietarias de su memoria. Las vistas
temporales de `combined` requieren que el decoder de render permanezca sin cambios
durante el packing; el worker actual respeta ese contrato.

## Reproducción y artefactos

- `experiments/copybench/README.md`: comandos y metodología.
- `experiments/copybench/worker.cjs` y `worker.mjs`: nuevo banco de pruebas.
- `experiments/copybench/summarize_worker.py`: resumen con sesiones individuales visibles.
- `build_copybench/worker_followup/`: ocho ejecuciones y `summary.json`.
- `build_copybench/directparts_verify/`: cuatro resultados de identidad.
- [Parche del candidato combinado](PACKING_COPY_CANDIDATE.patch).

Las carpetas de experimentos y builds están ignoradas por el repositorio y permanecen
locales. Estos son experimentos CPU; no miden la primera imagen, FPS ni pico de RAM
del viewer completo, y no son una validación en móviles u otras arquitecturas.
