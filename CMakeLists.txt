cmake_minimum_required(VERSION 3.14)

project(OWStreamRecordExRec)

add_library(OWStreamRecordExRec MODULE owstreamrecordexrec.cpp)

include_directories(libobs)

target_link_directories(OWStreamRecordExRec PUBLIC libs)

target_link_libraries(OWStreamRecordExRec obs)