# Kick's 3rdParty.cmake includes this from the AudIO tree to register
# AudIO's JUCE modules. The public AudIO release is a plain library
# without them; AudIO_juce wraps it and AudIO_constants stands in for the
# constants module that is still missing.
juce_add_modules(
    ${CMAKE_CURRENT_LIST_DIR}/AudIO_juce
    ${CMAKE_CURRENT_LIST_DIR}/AudIO_constants
)
