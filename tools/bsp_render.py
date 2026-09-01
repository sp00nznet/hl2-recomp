"""Render a map's real geometry straight off the disc.

The Xbox disc carries its ~120 BSPs uncompressed -- only the material and
model archives are behind xCompress -- so map geometry is readable without
booting the engine. That matters because it separates two questions that are
easy to confuse: whether the *data* is understood, and whether the recompiled
*engine* runs. This answers the first.

VBSP v19, four lumps:

    3   VERTEXES    float[3] per vertex
    7   FACES       dface_t, 56 bytes
    12  EDGES       two vertex indices
    13  SURFEDGES   signed edge index; negative means traverse it backwards

A face is a fan of surfedges, and the sign convention is the whole trick: a
positive surfedge takes the edge's first vertex, a negative one takes its
second, which is what keeps every face's winding consistent.

    py -3 tools/bsp_render.py game/GameMedia/maps/d1_canals_01.bsp out.bmp

ponytail: a z-buffer and flat shading, no textures or lightmaps. The question
is whether the geometry is right, and a shaded solid answers that -- a
wireframe would hide exactly the errors worth seeing.
"""
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT = 640, 480


def read_lumps(data):
    magic, version = struct.unpack_from("<4sI", data, 0)
    if magic != b"VBSP":
        raise SystemExit(f"not a VBSP file: {magic!r}")
    lumps = []
    for i in range(64):
        ofs, length, _ver, _fcc = struct.unpack_from("<iiI4s", data, 8 + i * 16)
        lumps.append((ofs, length))
    return version, lumps


def load_geometry(path):
    data = Path(path).read_bytes()
    version, lumps = read_lumps(data)

    vofs, vlen = lumps[3]
    verts = [struct.unpack_from("<fff", data, vofs + i * 12)
             for i in range(vlen // 12)]

    eofs, elen = lumps[12]
    edges = [struct.unpack_from("<HH", data, eofs + i * 4)
             for i in range(elen // 4)]

    sofs, slen = lumps[13]
    surfedges = [struct.unpack_from("<i", data, sofs + i * 4)[0]
                 for i in range(slen // 4)]

    fofs, flen = lumps[7]
    faces = []          # index lists into `verts`
    disp_faces = {}     # dispinfo index -> corner vertex positions
    for i in range(flen // 56):
        firstedge, numedges = struct.unpack_from("<ih", data, fofs + i * 56 + 4)
        dispinfo = struct.unpack_from("<h", data, fofs + i * 56 + 12)[0]
        if numedges < 3:
            continue
        poly = []
        for k in range(numedges):
            se = surfedges[firstedge + k]
            # Sign picks which end of the edge continues the loop.
            poly.append(edges[se][0] if se >= 0 else edges[-se][1])
        if dispinfo >= 0:
            # A displacement replaces its own face: the flat quad is a base
            # surface the terrain is built on, never drawn itself.
            if numedges == 4:
                disp_faces[dispinfo] = [verts[v] for v in poly]
            continue
        faces.append(poly)

    tris = expand_displacements(data, lumps, disp_faces)
    return version, verts, faces, tris


def expand_displacements(data, lumps, disp_faces):
    """Build terrain triangles from the displacement lumps.

    All of HL2's outdoor ground is displacements, not brush faces, so without
    this a canal map renders as walls around a hole. Each displacement takes a
    four-corner base face and subdivides it into a (2^power + 1) square grid,
    offsetting every grid point along its own stored direction.

    dispinfo is 176 bytes; the fields that matter are startPosition (0),
    m_iDispVertStart (12) and power (20). Each disp_vert is a direction and a
    distance, 20 bytes.

    startPosition identifies which corner of the base face is the grid's
    origin -- the face's winding alone does not say, and getting it wrong
    rotates every patch of terrain against its neighbours.
    """
    dofs, dlen = lumps[26]
    vofs, vlen = lumps[33]
    if not dlen or not vlen:
        return []

    dverts = [struct.unpack_from("<ffff", data, vofs + i * 20)
              for i in range(vlen // 20)]

    out = []
    for d in range(dlen // 176):
        base = dofs + d * 176
        sx, sy, sz = struct.unpack_from("<fff", data, base)
        vert_start = struct.unpack_from("<i", data, base + 12)[0]
        power = struct.unpack_from("<i", data, base + 20)[0]
        corners = disp_faces.get(d)
        if not corners or power < 1:
            continue

        # Rotate the winding so the corner nearest startPosition is first.
        best = min(range(4), key=lambda k: (corners[k][0] - sx) ** 2
                   + (corners[k][1] - sy) ** 2 + (corners[k][2] - sz) ** 2)
        c = corners[best:] + corners[:best]

        size = (1 << power) + 1
        grid = []
        for i in range(size):
            fi = i / (size - 1)
            row_a = [c[0][k] + (c[1][k] - c[0][k]) * fi for k in range(3)]
            row_b = [c[3][k] + (c[2][k] - c[3][k]) * fi for k in range(3)]
            for j in range(size):
                fj = j / (size - 1)
                p = [row_a[k] + (row_b[k] - row_a[k]) * fj for k in range(3)]
                dv = dverts[vert_start + i * size + j]
                grid.append((p[0] + dv[0] * dv[3],
                             p[1] + dv[1] * dv[3],
                             p[2] + dv[2] * dv[3]))

        for i in range(size - 1):
            for j in range(size - 1):
                a = grid[i * size + j]
                b = grid[(i + 1) * size + j]
                cc = grid[(i + 1) * size + j + 1]
                dd = grid[i * size + j + 1]
                out.append((a, b, cc))
                out.append((a, cc, dd))
    return out


def player_start(data):
    """Camera placed where the player spawns, from the entity lump.

    Rendering from outside shows only the sealed outer hull -- a Source map is
    a closed box, so from any exterior viewpoint every interesting surface is
    behind a wall. info_player_start is where the engine itself would put the
    camera, so it is the honest place to look from.

    The entity lump is plain text key/value blocks. Source angles are
    (pitch, yaw, roll) in degrees, and eye height is 64 units above the spawn
    origin, which is at the player's feet.
    """
    import re
    ofs, length = read_lumps(data)[1][0]
    text = data[ofs:ofs + length].decode("latin-1")
    for block in re.finditer(r"\{[^{}]*\}", text):
        body = block.group(0)
        if '"info_player_start"' not in body:
            continue
        keys = dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', body))
        origin = [float(v) for v in keys.get("origin", "0 0 0").split()]
        angles = [float(v) for v in keys.get("angles", "0 0 0").split()]
        origin[2] += 64.0
        return origin, angles
    return None, None


def render(verts, faces, tris, out_path, eye=None, angles=None):
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    cx, cy, cz = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, (min(zs) + max(zs)) / 2
    radius = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)) / 2 or 1.0

    import math
    if eye is not None:
        ex, eyy, ez = eye
        pitch, yaw = math.radians(angles[0]), math.radians(angles[1])
        # Source: +X forward at yaw 0, +Z up, pitch positive looks *down*.
        fwd = (math.cos(yaw) * math.cos(pitch),
               math.sin(yaw) * math.cos(pitch),
               -math.sin(pitch))
    else:
        dist = radius * 2.2
        ey, ep = math.radians(35.0), math.radians(28.0)
        ex = cx + dist * math.cos(ep) * math.cos(ey)
        eyy = cy + dist * math.cos(ep) * math.sin(ey)
        ez = cz + dist * math.sin(ep)
        fwd = (cx - ex, cy - eyy, cz - ez)
    fl = math.sqrt(sum(c * c for c in fwd)) or 1.0
    fwd = tuple(c / fl for c in fwd)
    # World up is +Z in Source.
    right = (fwd[1] * 1.0 - fwd[2] * 0.0, fwd[2] * 0.0 - fwd[0] * 1.0, 0.0)
    rl = math.sqrt(sum(c * c for c in right)) or 1.0
    right = tuple(c / rl for c in right)
    up = (right[1] * fwd[2] - right[2] * fwd[1],
          right[2] * fwd[0] - right[0] * fwd[2],
          right[0] * fwd[1] - right[1] * fwd[0])

    focal = WIDTH / 2 / math.tan(math.radians(35.0))
    colour = bytearray(WIDTH * HEIGHT * 3)
    depth = [1e30] * (WIDTH * HEIGHT)

    NEAR = 1.0

    def to_cam(v):
        d = (v[0] - ex, v[1] - eyy, v[2] - ez)
        return (sum(a * b for a, b in zip(d, right)),
                sum(a * b for a, b in zip(d, up)),
                sum(a * b for a, b in zip(d, fwd)))

    def clip_near(poly):
        """Sutherland-Hodgman against the near plane, in camera space.

        Dropping any face that crosses it -- the easy alternative -- deletes
        exactly the surfaces closest to the camera, which from inside a level
        is most of the room. That is what left the view full of holes.
        """
        out = []
        for i, cur in enumerate(poly):
            prv = poly[i - 1]
            cur_in, prv_in = cur[2] >= NEAR, prv[2] >= NEAR
            if cur_in != prv_in:
                t = (NEAR - prv[2]) / (cur[2] - prv[2])
                out.append((prv[0] + t * (cur[0] - prv[0]),
                            prv[1] + t * (cur[1] - prv[1]),
                            NEAR))
            if cur_in:
                out.append(cur)
        return out

    def project(c):
        return (WIDTH / 2 + focal * c[0] / c[2],
                HEIGHT / 2 - focal * c[1] / c[2],
                c[2])

    def tri(p0, p1, p2, shade):
        minx = max(int(min(p0[0], p1[0], p2[0])), 0)
        maxx = min(int(max(p0[0], p1[0], p2[0])) + 1, WIDTH)
        miny = max(int(min(p0[1], p1[1], p2[1])), 0)
        maxy = min(int(max(p0[1], p1[1], p2[1])) + 1, HEIGHT)
        if minx >= maxx or miny >= maxy:
            return
        area = ((p1[0] - p0[0]) * (p2[1] - p0[1])
                - (p2[0] - p0[0]) * (p1[1] - p0[1]))
        if abs(area) < 1e-9:
            return
        for y in range(miny, maxy):
            for x in range(minx, maxx):
                px, py = x + 0.5, y + 0.5
                w0 = ((p1[0] - p0[0]) * (py - p0[1])
                      - (px - p0[0]) * (p1[1] - p0[1])) / area
                w1 = ((px - p0[0]) * (p2[1] - p0[1])
                      - (p2[0] - p0[0]) * (py - p0[1])) / area
                w2 = 1.0 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w2 * p0[2] + w1 * p1[2] + w0 * p2[2]
                i = y * WIDTH + x
                if z >= depth[i]:
                    continue
                depth[i] = z
                o = i * 3
                colour[o] = shade[2]
                colour[o + 1] = shade[1]
                colour[o + 2] = shade[0]

    import math as _m
    drawn = 0
    # Brush faces as index lists, then displacement terrain as raw triangles.
    for pts in ([verts[i] for i in poly] for poly in faces):
        # Flat shade from the face normal, so surface orientation is visible.
        ux = (pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], pts[1][2] - pts[0][2])
        vx = (pts[2][0] - pts[0][0], pts[2][1] - pts[0][1], pts[2][2] - pts[0][2])
        n = (ux[1] * vx[2] - ux[2] * vx[1],
             ux[2] * vx[0] - ux[0] * vx[2],
             ux[0] * vx[1] - ux[1] * vx[0])
        nl = _m.sqrt(sum(c * c for c in n)) or 1.0
        lit = abs(sum(a * b for a, b in zip((c / nl for c in n), (0.4, 0.5, 0.75))))
        s = int(40 + 200 * min(lit, 1.0))
        shade = (s, int(s * 0.94), int(s * 0.86))

        # Backface cull. Without it the brush face the camera is standing
        # against fills the screen: from inside a sealed map, the faces looking
        # away from you are exactly the ones you should not see.
        to_eye = (ex - pts[0][0], eyy - pts[0][1], ez - pts[0][2])
        if sum(a * b for a, b in zip(n, to_eye)) <= 0.0:
            continue

        clipped = clip_near([to_cam(v) for v in pts])
        if len(clipped) < 3:
            continue
        proj = [project(c) for c in clipped]
        for k in range(1, len(proj) - 1):
            tri(proj[0], proj[k], proj[k + 1], shade)
        drawn += 1

    for pts in tris:
        ux = (pts[1][0] - pts[0][0], pts[1][1] - pts[0][1], pts[1][2] - pts[0][2])
        vx = (pts[2][0] - pts[0][0], pts[2][1] - pts[0][1], pts[2][2] - pts[0][2])
        n = (ux[1] * vx[2] - ux[2] * vx[1],
             ux[2] * vx[0] - ux[0] * vx[2],
             ux[0] * vx[1] - ux[1] * vx[0])
        nl = _m.sqrt(sum(cc * cc for cc in n)) or 1.0
        lit = abs(sum(a * b for a, b in zip((cc / nl for cc in n), (0.4, 0.5, 0.75))))
        s2 = int(30 + 190 * min(lit, 1.0))
        # Terrain tinted slightly green so it reads apart from brushwork.
        shade = (int(s2 * 0.82), int(s2 * 0.90), int(s2 * 0.70))
        clipped = clip_near([to_cam(v) for v in pts])
        if len(clipped) < 3:
            continue
        proj = [project(c) for c in clipped]
        for k in range(1, len(proj) - 1):
            tri(proj[0], proj[k], proj[k + 1], shade)
        drawn += 1

    row = (WIDTH * 3 + 3) & ~3
    pad = b"\x00" * (row - WIDTH * 3)
    body = b"".join(bytes(colour[y * WIDTH * 3:(y + 1) * WIDTH * 3]) + pad
                    for y in range(HEIGHT - 1, -1, -1))
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(body), 0, 0, 14 + 40)
    info = struct.pack("<IiiHHIIiiII", 40, WIDTH, HEIGHT, 1, 24, 0,
                       len(body), 2835, 2835, 0, 0)
    Path(out_path).write_bytes(header + info + body)
    return drawn


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    version, verts, faces, tris = load_geometry(sys.argv[1])
    data = Path(sys.argv[1]).read_bytes()
    eye, angles = player_start(data)
    drawn = render(verts, faces, tris, sys.argv[2], eye, angles)
    where = (f"from info_player_start {eye[0]:.0f},{eye[1]:.0f},{eye[2]:.0f} "
             f"yaw {angles[1]:.0f}" if eye else "from outside")
    print(f"VBSP v{version}: {len(verts)} vertices, {len(faces)} brush faces, "
          f"{len(tris)} terrain triangles, {drawn} drawn {where} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
