package dev.picoswitch.bridge.touch

/**
 * Small document builders shared by the touch test suites.
 *
 * Here rather than repeated per file so that every test which says "the same
 * layout with one control moved" means the same thing, and so a change to the
 * document model has one place to update instead of five.
 */

/** The shipped layout for [profile], unchanged. */
fun authored(profile: TouchControllerProfile): TouchLayoutDocument =
    TouchLayoutDocument.authoredDefault(profile)

/**
 * The shipped layout with one instance shifted by a normalized delta.
 *
 * Expressed as an absolute placement rather than through [TouchLayoutEditor.move]
 * because move deliberately clamps against the live rectangle, and a test that
 * wants a control in a stated place should say so rather than discover where the
 * clamp allowed it to land.
 */
fun nudged(
    profile: TouchControllerProfile,
    instanceId: String,
    deltaX: Float,
    deltaY: Float,
): TouchLayoutDocument {
    val document = authored(profile)
    val instance = requireNotNull(document.instance(instanceId)) {
        "$instanceId is not in the shipped ${profile.id} layout"
    }
    return TouchLayoutEditor.place(
        document,
        setOf(instanceId),
        instance.anchorX + deltaX,
        instance.anchorY + deltaY,
    )
}

/** The shipped layout with [instanceId] removed, as Delete would leave it. */
fun without(profile: TouchControllerProfile, instanceId: String): TouchLayoutDocument =
    TouchLayoutEditor.delete(authored(profile), setOf(instanceId), editGroup = false).document

/**
 * [document] with [instanceId] moved to exactly this anchor and no offset.
 *
 * The blunt instrument the geometry tests want: a control is at the stated
 * normalized point, with no cluster displacement and no clamping to argue with.
 */
fun pinned(
    document: TouchLayoutDocument,
    instanceId: String,
    anchorX: Float,
    anchorY: Float,
): TouchLayoutDocument = TouchLayoutEditor.place(document, setOf(instanceId), anchorX, anchorY)
    .let { placed ->
        placed.copy(
            controls = placed.controls.map {
                if (it.instanceId == instanceId) it.copy(offsetXUnits = 0f, offsetYUnits = 0f)
                else it
            },
        )
    }

/** The shipped layout plus a second instance of [catalogId], pinned to an anchor. */
fun withDuplicate(
    profile: TouchControllerProfile,
    catalogId: String,
    anchorX: Float,
    anchorY: Float,
): Pair<TouchLayoutDocument, String> {
    val added = TouchLayoutEditor.add(authored(profile), profile, catalogId, anchorX, anchorY)
    val created = added.created.single()
    return pinned(added.document, created, anchorX, anchorY) to created
}

/**
 * A layout containing ONLY the named instances, at the stated anchors.
 *
 * For tests about routing and aggregation rather than about the shipped
 * arrangement. Building on the full controller instead would make every such
 * test depend on where the authored layout happens to leave a gap, so a future
 * template nudge would break a suite that has nothing to do with geometry.
 *
 * Each entry is `catalogId to (anchorX to anchorY)`; repeating a catalog id
 * produces genuine duplicate instances with distinct ids, returned in order.
 */
fun documentOf(
    profile: TouchControllerProfile,
    vararg placements: Pair<String, Pair<Float, Float>>,
): Pair<TouchLayoutDocument, List<String>> {
    var document = TouchLayoutDocument(
        profileId = profile.id,
        templateId = profile.defaultTemplate.id,
        basedOnRevision = profile.defaultTemplate.templateRevision,
    )
    val ids = mutableListOf<String>()
    placements.forEach { (catalogId, anchor) ->
        val entry = requireNotNull(profile.catalogEntry(catalogId)) {
            "$catalogId is not in the ${profile.id} catalog"
        }
        val instanceId = TouchLayoutEditor.allocateInstanceId(document, catalogId)
        document = document.copy(
            controls = document.controls + TouchControlInstance(
                instanceId = instanceId,
                catalogId = entry.id,
                anchorX = anchor.first,
                anchorY = anchor.second,
                zIndex = document.controls.size,
            ),
        )
        ids += instanceId
    }
    return document to ids
}

/** The shipped layout plus every catalog control it does not place, at its authored spot. */
fun withOptionalControls(profile: TouchControllerProfile): TouchLayoutDocument {
    var document = authored(profile)
    profile.catalog.filterNot { it.inDefaultLayout }.forEach { entry ->
        document = TouchLayoutEditor.add(
            document, profile, entry.id, entry.geometry.anchorX, entry.geometry.anchorY,
        ).document
    }
    return document
}
