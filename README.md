# AC Super FX

Pack de efectos para OBS Studio, por alejoxcode.

## Efectos

- **AC Super FX Blur** (filtro): preajustes (Suave, Medio, Fuerte, Censura), intensidad de 0 a 96 px, estilo Suave o Rápido, y modo rectángulo para desenfocar solo una zona (o todo menos esa zona) con borde suave.
- **AC Super FX Fondo animado** (fuente): humo o gradiente animado generado por shader. Colores fijos, tomados de otra fuente de OBS, o de la canción que suena con Music Now Playing.
- **AC Super FX Marco** (filtro): esquinas redondeadas, borde y sombra paralela para webcams y ventanas.

### Integración con Music Now Playing

AC Super FX consulta el procedimiento global de OBS `music_now_playing_get_palette`, que devuelve `playing` (bool) y `accent`, `tint`, `background` (enteros `0xRRGGBB`). Si el procedimiento no existe o no suena nada, usa los colores fijos.

## Compilar en macOS

Requisitos: Xcode 16+, CMake 3.28+ (`brew install cmake`).

```sh
cmake --preset macos
cmake --build --preset macos
```

La primera vez descarga y compila las dependencias de OBS (tarda varios minutos).
El resultado queda en `build_macos/RelWithDebInfo/ac-super-fx.plugin`.

## Instalar

```sh
mkdir -p ~/Library/Application\ Support/obs-studio/plugins
cp -R build_macos/RelWithDebInfo/ac-super-fx.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

Reinicia OBS → clic derecho en una fuente → Filtros → "+" → **AC Super FX Blur**.

## Publicar una versión

Sube la versión en `buildspec.json` y empuja una rama `release/X.Y.Z`; GitHub Actions compila y publica la release.

## Estructura

- `src/plugin-main.c`: registro del módulo.
- `src/blur-filter.c`: filtro de blur (captura → pasada H → pasada V → dibujo).
- `data/effects/blur.effect`: shader.
- `data/locale/`: traducciones (en-US, es-ES).
