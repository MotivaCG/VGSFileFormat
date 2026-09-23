"""Scene Import translator for the VGS Capture object.

The Scene Import LOP only translates object types it has a translator for, and skips the
rest with a warning. A VGS Capture object is a geometry object with the capture's
controls on it, so it is translated exactly as a geometry object is: with whatever
translators are registered for 'geo' - Houdini's own, Karma's, and any a studio adds.
The splats inside then arrive in Solaris as a ParticleField3DGaussianSplat, as they do
through a SOP Import.
"""


class _FollowGeo(list):
    """The translators for 'vgs_capture': any registered for it directly, then those of
    'geo' as they stand when an object is translated.

    Looked up at that moment rather than copied now, because plugin files are only
    ordered within one folder: this package's folder may well be read before Houdini's
    own, when nothing is registered for 'geo' yet.
    """

    def __init__(self, manager):
        super().__init__()
        self._manager = manager

    def __iter__(self):
        yield from list.__iter__(self)
        geo = self._manager._translators.get('geo', [])
        if geo is not self:
            yield from geo


def registerTranslators(manager):
    if not isinstance(manager._translators.get('vgs_capture'), _FollowGeo):
        manager._translators['vgs_capture'] = _FollowGeo(manager)
