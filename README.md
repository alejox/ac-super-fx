# AC Super FX

Pack de efectos para OBS Studio, por alejoxcode.

## Efectos

- **AC Super FX Blur** (filtro): desenfoque Gaussiano o Box, radio de 0 a 96 px.

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

## Estructura

- `src/plugin-main.c`: registro del módulo.
- `src/blur-filter.c`: filtro de blur (captura → pasada H → pasada V → dibujo).
- `data/effects/blur.effect`: shader.
- `data/locale/`: traducciones (en-US, es-ES).
